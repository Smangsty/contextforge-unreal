"""Verify the UE 5.8 nested Blueprint cache compatibility without launching Unreal."""
import collections
import dataclasses
import runpy
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch


class FakeBlueprint:
    def __init__(self):
        self.variables = ["FP_MoveRight"]
        self.graphs = ["AnimGraph", "EventGraph"]
        self.dispatchers = ["OnMoved"]

    @staticmethod
    def cast(obj):
        if not isinstance(obj, FakeBlueprint):
            raise TypeError("Blueprint: Cannot cast type 'AnimStateNode' to 'Blueprint'")
        return obj


class FakeObject:
    def __init__(self, outer, name="Object"):
        self.outer = outer
        self.name = name

    def get_outer(self):
        return self.outer

    def get_name(self):
        return self.name

    def get_path_name(self):
        return self.name


class FakeEditorLibrary:
    @staticmethod
    def list_member_variable_names(blueprint, inherited):
        assert inherited is False
        return blueprint.variables

    @staticmethod
    def list_graphs(blueprint):
        return [FakeObject(None, name) for name in blueprint.graphs]

    @staticmethod
    def list_event_dispatchers(blueprint):
        return blueprint.dispatchers


@dataclasses.dataclass
class FakeCacheEntry:
    fingerprint: str
    type_ids_by_category: dict = dataclasses.field(
        default_factory=lambda: collections.defaultdict(list))


class FakeCache:
    _graph_node_types = {}
    invalidations = []

    @classmethod
    def get(cls, graph):
        blueprint = unreal.Blueprint.cast(graph.get_outer())
        return {"Legacy": ["unchanged"]}

    @staticmethod
    def _fingerprint(graph):
        blueprint = unreal.Blueprint.cast(graph.get_outer())
        return "original-top-level"

    @classmethod
    def list_nodes(cls, graph):
        return ["Variables|GetFP_MoveRight", "Utilities|Float|Clamp"]

    @classmethod
    def invalidate(cls, blueprint):
        cls.invalidations.append(blueprint)
        cls._graph_node_types.clear()


unreal = types.ModuleType("unreal")
unreal.Blueprint = FakeBlueprint
unreal.BlueprintEditorLibrary = FakeEditorLibrary


class NestedGraphCompatibilityTests(unittest.TestCase):
    def test_nested_graph_recovers_owner_without_regressing_top_level(self):
        helper = Path(__file__).resolve().parents[1] / (
            "unreal/Plugins/ContextForgeUnrealTools/Content/Python/"
            "contextforge_unreal_blueprint_compat.py")
        epic = types.ModuleType("editor_toolset.toolsets.blueprint")
        epic._BlueprintCache = FakeCache
        epic._CacheEntry = FakeCacheEntry
        epic._DISPATCHER_PREFIX = "Default|EventDispatcher"
        toolsets = types.ModuleType("editor_toolset.toolsets")
        toolsets.blueprint = epic
        package = types.ModuleType("editor_toolset")
        package.toolsets = toolsets

        bp = FakeBlueprint()
        top = FakeObject(bp, "AnimGraph")
        nested = FakeObject(
            FakeObject(FakeObject(top, "SM_FP_Grounded"), "AnimStateNode"),
            "Directional")

        with patch.dict(sys.modules, {
            "unreal": unreal,
            "editor_toolset": package,
            "editor_toolset.toolsets": toolsets,
            "editor_toolset.toolsets.blueprint": epic
        }):
            install = runpy.run_path(str(helper))["install"]
            with self.assertRaisesRegex(TypeError, "AnimStateNode"):
                FakeCache.get(nested)

            self.assertEqual(install(), "installed")
            self.assertEqual(install(), "already-installed")
            self.assertEqual(FakeCache.get(top), {"Legacy": ["unchanged"]})
            self.assertEqual(FakeCache._fingerprint(top), "original-top-level")

            found = FakeCache.get(nested)
            self.assertEqual(found["Variables"], ["Variables|GetFP_MoveRight"])
            self.assertEqual(found["Utilities|Float"], ["Utilities|Float|Clamp"])
            self.assertEqual(found["Default"], ["Default|EventDispatcherOnMoved"])
            self.assertIs(FakeCache.get(nested), found)
            self.assertIn("FP_MoveRight", FakeCache._fingerprint(nested))

            bp.variables.append("FP_MoveForward")
            refreshed = FakeCache.get(nested)
            self.assertIsNot(refreshed, found)
            self.assertEqual(FakeCache.invalidations, [bp])
            self.assertIn("FP_MoveForward", FakeCache._fingerprint(nested))


if __name__ == "__main__":
    unittest.main()
