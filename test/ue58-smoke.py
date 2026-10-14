"""Disposable UE 5.8 ContextForgeUnrealTools integration smoke.

Requires a disposable CFSmoke.uproject with ContextForgeUnrealTools loaded.
The full-trust Python portion also requires launching the editor with
CONTEXTFORGE_UNREAL_ENABLE_FULL_TRUST_PYTHON=1, an explicit opt-in.
This script creates no production assets and saves no Blueprint or map.
Look for CF_SMOKE ALL_TESTS_PASSED in Saved/Logs/CFSmoke.log.
"""
import traceback
import unreal


def record(value):
    unreal.log("CF_SMOKE " + str(value))


def must(condition, label):
    if not condition:
        raise AssertionError(label)
    record("PASS " + label)


try:
    project_path = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path()))
    must(project_path.replace("\\", "/").endswith("/CFSmoke.uproject"),
         "refuse non-disposable projects")

    native = getattr(unreal, "ContextForgeBlueprintTools", None)
    pie = getattr(unreal, "ContextForgePIETools", None)
    must(native is not None and pie is not None, "native Blueprint and PIE classes loaded")
    must(unreal.ToolsetRegistry.is_toolset_class_registered(native),
         "Blueprint toolset registered")
    must(unreal.ToolsetRegistry.is_toolset_class_registered(pie),
         "PIE toolset registered")

    import contextforge_unreal_scripting as scripting

    must(unreal.ToolsetRegistry.is_toolset_class_registered(
        scripting.ContextForgeUnrealScripting), "Python toolset registered")
    output = scripting._run_script("print('CF_PYTHON_STDOUT_OK')", project_path)
    must(output.success and output.attempted and
         "CF_PYTHON_STDOUT_OK" in output.stdout, "Python output actually captured")
    wrong = scripting._run_script(
        "print('this must not run')", "C:/DifferentProject/Other.uproject")
    must(not wrong.success and not wrong.attempted and
         "PROJECT_IDENTITY_MISMATCH" in wrong.exception,
         "cross-project Python execution rejected")
    failed = scripting._run_script(
        "raise ValueError('CF_EXCEPTION_OK')", project_path)
    must(not failed.success and failed.attempted and
         "CF_EXCEPTION_OK" in failed.exception, "Python exceptions captured")

    assets = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.Actor)
    blueprint = assets.create_asset(
        "BP_ContextForgeProbe", "/Game/CFProbe", unreal.Blueprint, factory)
    must(blueprint is not None, "disposable Blueprint created")
    bp_path = blueprint.get_path_name()

    base_interfaces = native.list_interfaces(bp_path)
    must(base_interfaces.success, "interface inspection returns success")
    interface_asset = assets.create_asset(
        "BPI_ContextForgeProbe", "/Game/CFProbe", unreal.Blueprint,
        unreal.BlueprintInterfaceFactory())
    must(interface_asset is not None, "disposable Blueprint interface created")
    interface_path = interface_asset.get_path_name()
    added = native.add_interface(bp_path, interface_path, False)
    must(added.mutation_applied and added.readback_verified,
         "Blueprint interface added and read back")
    duplicate = native.add_interface(bp_path, interface_path, False)
    must(not duplicate.mutation_applied and duplicate.readback_verified,
         "interface addition is idempotent")
    visible = native.list_interfaces(bp_path)
    must(visible.success and len(visible.interfaces) > 0,
         "implemented interface appears in inventory")
    removed = native.remove_interface(
        bp_path, interface_path, True, False, False)
    must(removed.mutation_applied and removed.readback_verified,
         "direct interface removed with functions preserved")
    invalid = native.add_interface(
        bp_path, "/Script/Engine.NotAnInterface", False)
    must(invalid.outcome == "INVALID_INTERFACE",
         "invalid interface rejected without mutation")

    abstract = native.add_component_verified(
        bp_path, "/Script/Engine.ActorComponent", "CFAbstractComponent", False)
    must(abstract.outcome == "ABSTRACT_COMPONENT_CLASS" and
         not abstract.mutation_attempted and not abstract.mutation_applied and
         not abstract.compile_attempted,
         "abstract component rejected before Unreal SCS allocation")
    component = native.add_component_verified(
        bp_path, "/Script/Engine.BoxComponent", "CFProbeComponent", False)
    must(component.mutation_applied and component.readback_verified and
         component.compile_attempted, "concrete component created, compiled and read back")
    again = native.add_component_verified(
        bp_path, "/Script/Engine.BoxComponent", "CFProbeComponent", False)
    must(not again.mutation_applied and again.readback_verified,
         "component addition idempotent")
    compile_receipt = native.compile_with_diagnostics(bp_path, False)
    must(compile_receipt.compile_attempted and compile_receipt.compile_succeeded,
         "Blueprint strict compiler executed")

    graph = native.get_graph_snapshot(bp_path, "EventGraph")
    must(graph.success and graph.fingerprint, "graph snapshot fingerprint returned")
    must(bool(graph.layout_fingerprint) and graph.node_geometry is not None,
         "layout snapshot includes coordinate fingerprint and geometry")
    layout_stale = native.format_graph_cluster(
        bp_path, "EventGraph", "stale", [], 120, 60, True, False)
    must(layout_stale.outcome == "STALE_OR_INVALID_LAYOUT" and
         not layout_stale.mutation_attempted,
         "stale layout refused without mutation")
    comment_stale = native.upsert_graph_comment(
        bp_path, "EventGraph", "stale", [], "Test", "",
        unreal.LinearColor(0.2, 0.3, 0.4, 1.0), False)
    must(comment_stale.outcome == "STALE_OR_INVALID_LAYOUT" and
         not comment_stale.mutation_attempted,
         "stale comment refused without mutation")
    stale = native.rewire_graph_connection(
        bp_path, "EventGraph", "stale-fingerprint",
        "none", "none", "none", "none", "none", "none", False)
    must(stale.outcome == "STALE_OR_INVALID_GRAPH" and
         not stale.mutation_applied, "stale graph mutation refused")

    mismatch_map = pie.start_pie("/Game/CFProbe/NonexistentMap", 1, False)
    must(mismatch_map.outcome == "TEST_MAP_NOT_READY" and
         not mismatch_map.start_requested, "PIE refuses a different map")
    unowned = pie.stop_pie()
    must(unowned.outcome == "UNOWNED_SESSION",
         "PIE stop refuses sessions not owned by toolset")

    record("ALL_TESTS_PASSED")
except Exception:
    record("FAILED\n" + traceback.format_exc())
    raise
