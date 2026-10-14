"""Isolated UE 5.8 native precision graph-editing proof, no assets saved.

Run ONLY against disposable CFSmoke.uproject with a prebuilt staged plugin.
Never point this script at Project_TIB or another production Unreal project.
"""
import traceback
import unreal

def record(msg):
    unreal.log("CF_PRECISION " + str(msg))

def must(cond, label):
    if not cond:
        raise AssertionError(label)
    record("PASS " + label)

def by_guid(snapshot):
    return {str(n.node_guid): n for n in snapshot.node_geometry}

def fingerprint_same(before, after):
    return (str(before.fingerprint) == str(after.fingerprint)
            and sorted(map(str, before.connections)) == sorted(map(str, after.connections))
            and sorted(map(str, before.pin_details)) == sorted(map(str, after.pin_details)))

try:
    project = str(unreal.Paths.convert_relative_path_to_full(
        unreal.Paths.get_project_file_path())).replace("\\", "/")
    must(project.endswith("/CFSmoke.uproject"), "isolated smoke project enforced")
    native = getattr(unreal, "ContextForgeBlueprintTools", None)
    must(native is not None, "native graph toolset present")
    must(hasattr(native, "place_graph_nodes")
         and hasattr(native, "set_graph_comment"),
         "precision editing methods exposed through native UE reflection")
    must(hasattr(unreal, "CFGraphNodePlacement"), "placement USTRUCT reflected")
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.Actor)
    bp = asset_tools.create_asset("BP_PrecisionProbe", "/Game/CFProbe",
                                  unreal.Blueprint, factory)
    must(bp is not None, "unsaved Blueprint fixture created")
    name = bp.get_path_name()
    before = native.get_graph_snapshot(name, "EventGraph")
    must(before.success and len(before.node_guids) >= 2,
         "at least two fixture nodes available for cluster tests")
    ids = list(map(str, before.node_guids))
    original = by_guid(before)
    pts = [(i * 560, 0) for i in range(len(ids))]
    ordered = [unreal.CFGraphNodePlacement(node_guid=guid, x=pt[0], y=pt[1])
               for guid, pt in zip(ids, pts)]

    strict = native.place_graph_nodes(name, "EventGraph",
                                      before.layout_fingerprint, ordered,
                                      False, True, False)
    must(strict.outcome == "NODE_GEOMETRY_NOT_MEASURED" and
         not strict.mutation_attempted,
         "strict mode refuses headless unmeasured node geometry")
    deliberate = native.place_graph_nodes(name, "EventGraph",
                                           before.layout_fingerprint, ordered,
                                           False, False, False)
    record("PLACE_OUTCOME=" + str(deliberate.outcome) +
           " DETAIL=" + str(deliberate.detail))
    must(deliberate.readback_verified and deliberate.outcome in
         ("APPLIED_VERIFIED", "ALREADY_SATISFIED"),
         "exact batch placement verified")

    positioned = native.get_graph_snapshot(name, "EventGraph")
    must(fingerprint_same(before, positioned) and
         sorted(map(str, positioned.node_guids)) == sorted(ids),
         "graph wiring and all node identities preserved")
    placed = by_guid(positioned)
    must(all(placed[guid].x == target[0] and placed[guid].y == target[1]
             for guid, target in zip(ids, pts)),
         "every node at exactly user-chosen graph coordinates")
    must(len(positioned.node_geometry) == len(before.node_geometry),
         "no nodes introduced by placement")

    repeated = native.place_graph_nodes(name, "EventGraph",
                                         positioned.layout_fingerprint, ordered,
                                         False, False, False)
    must(repeated.outcome == "ALREADY_SATISFIED" and
         not repeated.mutation_applied and repeated.readback_verified,
         "same placement is a no-op")
    stale = native.place_graph_nodes(name, "EventGraph", "stale",
                                     ordered, False, False, False)
    must(stale.outcome == "STALE_OR_INVALID_LAYOUT" and
         not stale.mutation_attempted,
         "stale layout cannot mutate graph")
    collision = [
        unreal.CFGraphNodePlacement(node_guid=ids[0], x=pts[1][0], y=0)
    ]
    refused = native.place_graph_nodes(name, "EventGraph",
                                       positioned.layout_fingerprint, collision,
                                       False, False, False)
    must(refused.outcome == "PLACEMENT_COLLIDES_WITH_UNSELECTED" and
         not refused.mutation_attempted,
         "node collision detected before mutation")

    color = unreal.LinearColor(0.12, 0.43, 0.79, 1)
    outer = native.set_graph_comment(
        name, "EventGraph", positioned.layout_fingerprint, "",
        ids[:2], "Crouch State", "Control crouch and uncrouch.",
        color, -70, -120, 1000, 360, 24,
        True, True, True, False)
    record("OUTER_COMMENT_OUTCOME=" + str(outer.outcome) +
           " DETAIL=" + str(outer.detail))
    must(outer.readback_verified and bool(outer.result_node_guid)
         and outer.outcome == "APPLIED_VERIFIED",
         "styled comment wraps explicit two-node cluster")
    wrapped = native.get_graph_snapshot(name, "EventGraph")
    must(fingerprint_same(before, wrapped) and
         len(wrapped.node_guids) == len(before.node_guids) + 1,
         "comment creation doesn't alter existing graph wiring")
    info = by_guid(wrapped)[str(outer.result_node_guid)]
    must(info.comment_text == "Crouch State" and
         info.comment_details == "Control crouch and uncrouch." and
         info.comment_font_size == 24 and
         info.comment_moves_contained_nodes and
         info.comment_bubble_visible and info.comment_bubble_colored,
         "existing comment reader exposes title, details, font and settings")
    must(sorted(map(str, info.geometric_contained_node_guids)) == sorted(ids[:2]),
         "comment readback distinguishes geometric membership")
    must(sorted(map(str, info.comment_drag_member_guids)) == sorted(ids[:2]),
         "reader also exposes Unreal's distinct transient drag membership")
    must(info.x == -70 and info.y == -120 and
         info.width == 1000 and info.height == 360 and
         abs(info.comment_color.r - 0.12) < 0.001 and
         abs(info.comment_color.g - 0.43) < 0.001,
         "comment reader reports exact bounds and color")

    repeat_comment = native.set_graph_comment(
        name, "EventGraph", wrapped.layout_fingerprint,
        str(outer.result_node_guid), ids[:2], "Crouch State",
        "Control crouch and uncrouch.", color,
        -70, -120, 1000, 360, 24, True, True, True, False)
    must(repeat_comment.outcome == "ALREADY_SATISFIED"
         and not repeat_comment.mutation_applied and repeat_comment.readback_verified,
         "same comment settings are idempotent")

    # Explicit nested comment with precise independent styling/bounds.
    inner_color = unreal.LinearColor(0.76, 0.26, 0.27, 1)
    inner = native.set_graph_comment(
        name, "EventGraph", wrapped.layout_fingerprint, "",
        [ids[0]], "Input", "Read and forward player input.",
        inner_color, -30, -65, 320, 240, 16,
        False, False, False, False)
    record("NESTED_COMMENT_OUTCOME=" + str(inner.outcome))
    must(inner.readback_verified and inner.outcome == "APPLIED_VERIFIED",
         "nested subcluster comment creates without modifying parent")
    nested = native.get_graph_snapshot(name, "EventGraph")
    parent = by_guid(nested)[str(outer.result_node_guid)]
    child = by_guid(nested)[str(inner.result_node_guid)]
    must(str(inner.result_node_guid) in
         list(map(str, parent.geometric_contained_node_guids)) and
         str(outer.result_node_guid) in
         list(map(str, child.geometric_parent_comment_guids)),
         "reader reports nested comment hierarchy")
    must(not child.comment_moves_contained_nodes and
         not child.comment_bubble_visible and child.comment_font_size == 16,
         "nested comment retains own styling and group mode")
    blocked = native.place_graph_nodes(
        name, "EventGraph", nested.layout_fingerprint,
        [unreal.CFGraphNodePlacement(node_guid=ids[0], x=12000, y=0)],
        False, False, False)
    must(blocked.outcome == "COMMENT_MEMBERSHIP_WOULD_CHANGE" and
         not blocked.mutation_attempted,
         "moving grouped nodes out of comment requires explicit consent")
    rejected_comment = native.set_graph_comment(
        name, "EventGraph", nested.layout_fingerprint, "",
        [ids[0]], "Unrelated", "", color, -60, -100, 1100, 350,
        20, True, True, True, False)
    must(rejected_comment.outcome == "COMMENT_WOULD_ENCLOSE_OR_OVERLAP_UNSELECTED"
         and not rejected_comment.mutation_attempted,
         "unreviewed overlapping comment region rejected before mutation")

    # Restyle existing parent without moving nodes or destroying the nested region.
    new_color = unreal.LinearColor(0.36, 0.65, 0.25, 1)
    changed = native.set_graph_comment(
        name, "EventGraph", nested.layout_fingerprint,
        str(outer.result_node_guid), ids[:2] + [str(inner.result_node_guid)],
        "Crouch / UnCrouch", "Read and update player crouch state.",
        new_color, -75, -130, 1010, 370, 21,
        False, False, False, False)
    record("RESTYLE_OUTCOME=" + str(changed.outcome))
    must(changed.outcome == "APPLIED_VERIFIED" and changed.readback_verified,
         "existing comment restyled and resized with explicit nested contents")
    updated = native.get_graph_snapshot(name, "EventGraph")
    new_parent = by_guid(updated)[str(outer.result_node_guid)]
    must(new_parent.comment_text == "Crouch / UnCrouch" and
         new_parent.comment_details == "Read and update player crouch state." and
         new_parent.comment_font_size == 21 and
         not new_parent.comment_moves_contained_nodes and
         not new_parent.comment_bubble_visible and
         abs(new_parent.comment_color.g - 0.65) < 0.001,
         "restyled comment metadata rereads exactly")
    must(sorted(map(str, new_parent.geometric_contained_node_guids)) ==
         sorted(ids[:2] + [str(inner.result_node_guid)]),
         "restyling parent preserves nested comment and node membership")

    final = native.get_graph_snapshot(name, "EventGraph")
    must(fingerprint_same(before, final) and
         final.layout_fingerprint == updated.layout_fingerprint,
         "invalid and repeated operations preserve final logic")
    must(all(by_guid(final)[guid].x == pts[i][0] and
             by_guid(final)[guid].y == pts[i][1]
             for i, guid in enumerate(ids)),
         "comment edits did not move any nodes")
    record("ALL_TESTS_PASSED")
except Exception:
    record("FAILED\n" + traceback.format_exc())
    raise
