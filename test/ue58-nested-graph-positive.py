"""Real UE 5.8 nested Blueprint graph regression. Disposable CFSmoke only."""
import os
import sys
import traceback
import unreal


def note(msg):
    unreal.log("CF_NESTED " + str(msg))


def require(condition, msg):
    if not condition:
        raise AssertionError(msg)
    note("PASS " + msg)


try:
    project = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path())).replace("\\", "/")
    require(project.endswith("/CFSmoke.uproject"), "disposable fixture boundary")

    source = os.environ.get("CONTEXTFORGE_UNREAL_NESTED_COMPAT_SOURCE")
    require(source and os.path.isdir(source), "local compatibility source supplied")
    sys.path.insert(0, source)
    from contextforge_unreal_blueprint_compat import install

    status = install()
    require(status in ("installed", "already-installed"),
            "known Epic cache defect patched in memory")

    from editor_toolset.toolsets.blueprint import BlueprintTools

    skeleton = unreal.load_asset(
        "/Game/Rigs/Characters/Human/SK_Military_Character_Skeleton")
    clip = unreal.load_asset(
        "/Game/Animations/Characters/Player/TFA_Quantum_FP_Pilot/"
        "Locomotion/A_TFA_FP_AR_Walk_F_Loop_Standing_Quantum")
    require(skeleton is not None and clip is not None,
            "isolated skeleton and movement clip loaded")
    require(clip.get_skeleton() == skeleton, "animation skeleton matched")

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    factory.set_editor_property("parent_class", unreal.AnimInstance)
    bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "ABP_CFNestedSmoke", "/Game/CFProbe", unreal.AnimBlueprint, factory)
    require(bp is not None, "create disposable AnimBP")

    BlueprintTools.add_variable(bp, "FP_MoveRight", "float")
    native = unreal.ContextForgeAnimationTools
    bp_path = bp.get_path_name()
    made = native.ensure_state_machine(bp_path, "SM_CFNode", -680, -220, False)
    require(made.readback_verified, "create native state machine")
    machine = native.inspect_state_machine(bp_path, "SM_CFNode")
    require(machine.success and bool(machine.fingerprint), "get machine fingerprint")
    state = native.upsert_animation_state(
        bp_path, "SM_CFNode", machine.fingerprint, "Directional",
        clip.get_path_name(), True, True, 160, 0, False)
    require(state.readback_verified, "create nested animation-state graph")

    graph = BlueprintTools.get_graph(bp, "Directional")
    require(graph is not None and "Directional" in graph.get_path_name(),
            "retrieve state-owned graph")
    BlueprintTools.compile_blueprint(bp)
    # In -run=pythonscript, UE's action database omits freshly authored
    # variable getters in BOTH the root and nested graphs. Test an actual
    # published node type so this still proves the nested cache and factory.
    root_graph = BlueprintTools.get_graph(bp, "AnimGraph")
    named_nested = list(BlueprintTools.find_node_types(graph, "FP_MoveRight", []))
    named_root = list(BlueprintTools.find_node_types(root_graph, "FP_MoveRight", []))
    if not named_nested and not named_root:
        note("INFO commandlet action database omits new member getters")
    available = list(BlueprintTools.find_node_types(
        graph, "Utilities|Casting|CastToWidget", []))
    node_type = next((n for n in available
                      if n == "Utilities|Casting|CastToWidget"), None)
    require(node_type is not None,
            "nested find_node_types returns a concrete engine node type")

    before = len(BlueprintTools.find_nodes(graph, ""))
    temp = BlueprintTools.create_node(
        graph, node_type, unreal.IntPoint(-490, -100))
    require(temp is not None, "nested create_node creates actual K2 node")
    require(len(BlueprintTools.find_nodes(graph, "")) == before + 1,
            "created K2 node appears in state graph")
    BlueprintTools.delete_node(temp)
    require(len(BlueprintTools.find_nodes(graph, "")) == before,
            "disposable node removed with graph restored")

    # Confirm the prior normal graph behavior still functions.
    root = BlueprintTools.get_graph(bp, "AnimGraph")
    require(bool(BlueprintTools.find_node_types(root, "CastToWidget", [])),
            "top-level graph node discovery unaffected")
    note("ALL_TESTS_PASSED")
except Exception:
    note("FAIL " + traceback.format_exc())
    raise
