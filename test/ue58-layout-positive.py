"""Positive Blueprint graph-format and comment smoke, disposable CFSmoke only.

Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>, against the
isolated CFSmoke fixture and prebuilt ContextForgeUnrealTools test plugin.
No project files or Blueprints are saved.
"""
import traceback
import unreal

def record(message):
    unreal.log("CF_LAYOUT " + str(message))

def must(condition, label):
    if not condition:
        raise AssertionError(label)
    record("PASS " + label)

try:
    path = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path()))
    must(path.replace("\\", "/").endswith("/CFSmoke.uproject"),
         "refuse non-disposable Unreal project")

    native = getattr(unreal, "ContextForgeBlueprintTools", None)
    must(native is not None, "native layout toolset available")
    assets = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.Actor)
    bp = assets.create_asset("BP_LayoutProbe", "/Game/CFProbe",
                             unreal.Blueprint, factory)
    must(bp is not None, "unsaved disposable Blueprint created")
    bp_path = bp.get_path_name()

    before = native.get_graph_snapshot(bp_path, "EventGraph")
    must(before.success and bool(before.layout_fingerprint),
         "geometry snapshot succeeds")
    record("NODE_COUNT=" + str(len(before.node_guids)))
    must(len(before.node_guids) > 0, "Blueprint provides event nodes to arrange")
    must(len(before.node_guids) == len(before.node_geometry),
         "geometry inventory covers every node")

    strict = native.format_graph_cluster(
        bp_path, "EventGraph", before.layout_fingerprint,
        before.node_guids, 120, 60, True, False)
    must(strict.outcome == "NODE_GEOMETRY_NOT_MEASURED"
         and not strict.mutation_attempted,
         "headless exact-only layout refuses unavailable Slate geometry")

    arranged = native.format_graph_cluster(
        bp_path, "EventGraph", before.layout_fingerprint,
        before.node_guids)
    record("FORMAT_OUTCOME=" + str(arranged.outcome))
    must(arranged.readback_verified and arranged.outcome in
         ("APPLIED_VERIFIED", "ALREADY_SATISFIED"),
         "cluster arranged with explicit estimated-geometry fallback")
    after = native.get_graph_snapshot(bp_path, "EventGraph")
    must(after.fingerprint == before.fingerprint
         and sorted(after.connections) == sorted(before.connections)
         and sorted(after.node_guids) == sorted(before.node_guids),
         "node identity and wiring preserved")
    must(all(n.width > 0 and n.height > 0 for n in after.node_geometry),
         "all nodes report positive dimensions")

    color = unreal.LinearColor(0.2, 0.5, 0.8, 1.0)
    wrapped = native.upsert_graph_comment(
        bp_path, "EventGraph", after.layout_fingerprint,
        before.node_guids, "Gameplay Event Logic", "", color, False)
    record("COMMENT_OUTCOME=" + str(wrapped.outcome))
    must(wrapped.readback_verified and bool(wrapped.result_node_guid),
         "named comment created around selected cluster")
    snap_comment = native.get_graph_snapshot(bp_path, "EventGraph")
    must(sorted(snap_comment.connections) == sorted(before.connections)
         and len(snap_comment.node_guids) == len(before.node_guids) + 1,
         "comment wrapping preserves all wiring and adds exactly one comment")

    repeated = native.upsert_graph_comment(
        bp_path, "EventGraph", snap_comment.layout_fingerprint,
        before.node_guids, "Gameplay Event Logic",
        wrapped.result_node_guid, color, False)
    record("COMMENT_REPEAT_OUTCOME=" + str(repeated.outcome))
    must(repeated.outcome == "ALREADY_SATISFIED"
         and repeated.readback_verified and not repeated.mutation_applied,
         "re-running comment update is idempotent")
    final = native.get_graph_snapshot(bp_path, "EventGraph")
    must(final.layout_fingerprint == snap_comment.layout_fingerprint,
         "idempotent update makes no layout changes")
    record("ALL_TESTS_PASSED")
except Exception:
    record("FAILED\\n" + traceback.format_exc())
    raise
