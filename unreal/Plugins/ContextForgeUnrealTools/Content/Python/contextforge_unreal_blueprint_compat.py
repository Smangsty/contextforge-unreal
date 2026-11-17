"""Compatibility for UE 5.8 Blueprint node discovery in nested animation graphs.

Epic's EditorToolset _BlueprintCache assumes an EdGraph's immediate outer is a
Blueprint. An animation state's graph instead belongs to an AnimStateNode,
which makes Blueprint.cast(graph.get_outer()) raise before any node is made.

Only replace the two affected cache helpers in memory. Engine files and the
existing registered tool names/schemas remain unchanged.
"""
import inspect

import unreal


def _owning_blueprint(graph):
    """Follow UObject outers until the real Blueprint is found."""
    outer = graph
    for _ in range(32):
        if outer is None:
            return None
        if isinstance(outer, unreal.Blueprint):
            return outer
        if not hasattr(outer, "get_outer"):
            return None
        next_outer = outer.get_outer()
        if next_outer is outer:
            return None
        outer = next_outer
    return None


def install():
    """Install once, only for the known UE 5.8 implementation defect."""
    from editor_toolset.toolsets import blueprint as epic

    cache = epic._BlueprintCache
    if getattr(cache, "_contextforge_nested_graph_compat", False):
        return "already-installed"

    original_get = cache.get
    original_fingerprint = cache._fingerprint
    source = inspect.getsource(original_get.__func__)
    fingerprint_source = inspect.getsource(original_fingerprint)
    known_defect = "unreal.Blueprint.cast(graph.get_outer())"
    if known_defect not in source or known_defect not in fingerprint_source:
        # Epic may have fixed or changed its implementation. Do not override it.
        return "upstream-differs"

    def fingerprint(graph):
        blueprint = _owning_blueprint(graph)
        if blueprint is None or isinstance(graph.get_outer(), unreal.Blueprint):
            return original_fingerprint(graph)
        names = sorted(str(v) for v in
                       unreal.BlueprintEditorLibrary.list_member_variable_names(
                           blueprint, False))
        graphs = sorted(g.get_name() for g in
                        unreal.BlueprintEditorLibrary.list_graphs(blueprint))
        return f"{names}|{graphs}"

    def get(cls, graph):
        blueprint = _owning_blueprint(graph)
        if blueprint is None or isinstance(graph.get_outer(), unreal.Blueprint):
            return original_get(graph)

        graph_key = graph.get_path_name()
        current_fingerprint = cls._fingerprint(graph)
        entry = cls._graph_node_types.get(graph_key)
        if entry is not None and entry.fingerprint == current_fingerprint:
            return entry.type_ids_by_category
        if entry is not None:
            cls.invalidate(blueprint)

        entry = epic._CacheEntry(fingerprint=current_fingerprint)
        for node_type in cls.list_nodes(graph):
            entry.type_ids_by_category[
                "|".join(node_type.split("|")[:-1])
            ].append(node_type)
        for name in unreal.BlueprintEditorLibrary.list_event_dispatchers(blueprint):
            entry.type_ids_by_category["Default"].append(
                f"{epic._DISPATCHER_PREFIX}{name}"
            )
        cls._graph_node_types[graph_key] = entry
        return entry.type_ids_by_category

    cache._fingerprint = staticmethod(fingerprint)
    cache.get = classmethod(get)
    cache._contextforge_nested_graph_compat = True
    return "installed"
