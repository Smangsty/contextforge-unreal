"""Native UE 5.8 animation graph test. Only a disposable CFSmoke.uproject is allowed."""
import traceback
import unreal

def log(message):
    unreal.log("CF_ANIM " + str(message))

def must(condition, message):
    if not condition:
        raise AssertionError(message)
    log("PASS " + message)

try:
    project = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path())).replace("\\", "/")
    must(project.endswith("/CFSmoke.uproject"), "disposable project boundary")
    native = getattr(unreal, "ContextForgeAnimationTools", None)
    must(native is not None, "new native animation toolset loaded")
    must(unreal.ToolsetRegistry.is_toolset_class_registered(native),
         "new animation toolset registered")

    skeleton = unreal.load_asset("/Game/Rigs/Characters/Human/SK_Military_Character_Skeleton")
    clip = unreal.load_asset(
        "/Game/Animations/Characters/Player/TFA_Quantum_FP_Pilot/Locomotion/"
        "A_TFA_FP_AR_Walk_F_Loop_Standing_Quantum")
    must(skeleton is not None and clip is not None,
         "isolated copies of original skeleton and animation load")
    must(clip.get_skeleton() == skeleton, "copy clip references matching skeleton")

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    factory.set_editor_property("parent_class", unreal.AnimInstance)
    bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "ABP_CFAnimSmoke", "/Game/CFProbe", unreal.AnimBlueprint, factory)
    must(bp is not None, "create unsaved test Animation Blueprint")
    bp_path = bp.get_path_name()
    missing = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    must(not missing.success, "missing machine returns a clear failure")

    create = native.ensure_state_machine(bp_path, "SM_CFLocomotion", -680, -240, False)
    log("ensure outcome=" + str(create.outcome) + " detail=" + str(create.detail))
    must(create.readback_verified, "create state machine through new native API")

    machine = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    must(machine.success and machine.fingerprint, "machine returns stable fingerprint")

    stale = native.upsert_animation_state(
        bp_path, "SM_CFLocomotion", "stale-fingerprint", "Standing",
        clip.get_path_name(), True, True, 160, 0, False)
    must(stale.outcome == "STALE_MACHINE_FINGERPRINT" and
         not stale.mutation_attempted, "reject stale state authoring precondition")

    standing = native.upsert_animation_state(
        bp_path, "SM_CFLocomotion", machine.fingerprint, "Standing",
        clip.get_path_name(), True, True, 160, 0, False)
    log("standing outcome=" + str(standing.outcome) + " detail=" + str(standing.detail))
    must(standing.readback_verified, "create entry state with assigned sequence")

    machine = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    must(machine.has_entry and len(machine.states) == 1 and
         machine.states[0].pose_connected and machine.states[0].entry,
         "entry state has an animated pose wired to its result")

    moving = native.upsert_animation_state(
        bp_path, "SM_CFLocomotion", machine.fingerprint, "Moving",
        clip.get_path_name(), True, False, 680, 0, False)
    log("moving outcome=" + str(moving.outcome) + " detail=" + str(moving.detail))
    must(moving.readback_verified, "create second state with sequence")

    machine = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    must(len(machine.states) == 2 and all(s.pose_connected for s in machine.states),
         "both states have connected clip players")

    invalid = native.upsert_state_transition(
        bp_path, "SM_CFLocomotion", machine.fingerprint,
        "Standing", "Moving", "DefinitelyNotABoolean", False, 0.3, False)
    must(invalid.outcome == "INVALID_RULE_OR_BLEND" and
         not invalid.mutation_attempted, "invalid bool rule rejected")

    # The inherited AnimInstance property is used ONLY in this smoke test.
    # The actual game will bind explicit Mover-derived AnimBP booleans.
    rule_var = "bUseMultiThreadedAnimationUpdate"
    forward = native.upsert_state_transition(
        bp_path, "SM_CFLocomotion", machine.fingerprint,
        "Standing", "Moving", rule_var, False, 0.3, False)
    log("forward outcome=" + str(forward.outcome) + " detail=" + str(forward.detail))
    must(forward.readback_verified, "create boolean-driven transition")

    machine = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    reverse = native.upsert_state_transition(
        bp_path, "SM_CFLocomotion", machine.fingerprint,
        "Moving", "Standing", rule_var, True, 0.35, False)
    log("reverse outcome=" + str(reverse.outcome) + " detail=" + str(reverse.detail))
    must(reverse.readback_verified, "create negated return transition")

    machine = native.inspect_state_machine(bp_path, "SM_CFLocomotion")
    must(len(machine.transitions) == 2 and
         all(t.rule_connected for t in machine.transitions),
         "two correct transition rule graphs read back")

    validation = native.validate_state_machine(
        bp_path, "SM_CFLocomotion", True)
    log("validation structure=" + str(validation.structure_valid) +
        " compile=" + str(validation.compile_succeeded) +
        " issues=" + str(list(validation.issues)))
    must(validation.compile_attempted and validation.compile_succeeded,
         "native Unreal animation Blueprint compiler succeeds")
    must(not validation.structure_valid and
         any("pose output" in str(i).lower() for i in validation.issues),
         "validation detects intentionally unconnected machine output")
    log("ALL_TESTS_PASSED")
except Exception:
    log("FAILED\n" + traceback.format_exc())
    raise
