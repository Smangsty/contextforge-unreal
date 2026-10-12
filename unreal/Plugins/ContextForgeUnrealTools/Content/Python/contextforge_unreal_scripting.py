"""Controlled editor-side Python execution with truthful, project-bound receipts.

This is a *WRITE/full-trust* capability. ContextForge must authorize the exact
code arguments for every call. It is not a read-only escape hatch or a sandbox.
"""
import contextlib
import io
import ntpath
import traceback

import unreal
import toolset_registry


MAX_CODE_CHARACTERS = 65536
MAX_CAPTURE_CHARACTERS = 16384


def _project_key(path: str) -> str:
    if not isinstance(path, str) or not path or not ntpath.isabs(path):
        return ""
    if "\x00" in path or "\n" in path or "\r" in path:
        return ""
    return ntpath.normcase(ntpath.normpath(path)).casefold()


def _bounded(value: str):
    return value[:MAX_CAPTURE_CHARACTERS], len(value) > MAX_CAPTURE_CHARACTERS


@unreal.ustruct()
class ContextForgePythonReceipt(unreal.StructBase):
    """Editor-side execution proof. A successful transport is not execution success."""
    success = unreal.uproperty(bool)
    attempted = unreal.uproperty(bool)
    project_path = unreal.uproperty(str)
    stdout = unreal.uproperty(str)
    stderr = unreal.uproperty(str)
    exception = unreal.uproperty(str)
    output_truncated = unreal.uproperty(bool)


def _receipt(success, attempted, project, stdout="", stderr="", exception=""):
    out, out_cut = _bounded(stdout)
    err, err_cut = _bounded(stderr)
    exc, exc_cut = _bounded(exception)
    # UE 5.8 dynamic @ustruct constructors silently ignore keyword values.
    # Set each UPROPERTY explicitly or every receipt falsely reports defaults.
    receipt = ContextForgePythonReceipt()
    receipt.success = bool(success)
    receipt.attempted = bool(attempted)
    receipt.project_path = project
    receipt.stdout = out
    receipt.stderr = err
    receipt.exception = exc
    receipt.output_truncated = out_cut or err_cut or exc_cut
    return receipt


def _run_script(code: str, expected_project_path: str):
    project = str(unreal.Paths.convert_relative_path_to_full(unreal.Paths.get_project_file_path()) or "")
    if not _project_key(project) or not _project_key(expected_project_path):
        return _receipt(False, False, project, exception="PROJECT_PATH_UNAVAILABLE_OR_INVALID")
    if _project_key(project) != _project_key(expected_project_path):
        return _receipt(False, False, project, exception="PROJECT_IDENTITY_MISMATCH")
    if not isinstance(code, str) or not code.strip() or len(code) > MAX_CODE_CHARACTERS:
        return _receipt(False, False, project, exception="PYTHON_SOURCE_INVALID_OR_TOO_LARGE")

    stdout = io.StringIO()
    stderr = io.StringIO()
    scope = {"__name__": "__contextforge_unreal__", "unreal": unreal}
    try:
        # Editor MCP calls are serialized onto the game thread by Epic.
        # Python work has no safe forced cancellation/timeout in that context.
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            exec(compile(code, "<ContextForgeUnrealScript>", "exec"), scope, scope)
        return _receipt(True, True, project, stdout.getvalue(), stderr.getvalue())
    except BaseException:
        return _receipt(
            False, True, project, stdout.getvalue(), stderr.getvalue(),
            traceback.format_exc(),
        )


@unreal.uclass()
class ContextForgeUnrealScripting(unreal.ToolsetDefinition):
    """Execute explicitly authorized Python inside the identified Unreal Editor."""

    @toolset_registry.tool_call
    @staticmethod
    def execute_python(code: str, expected_project_path: str) -> ContextForgePythonReceipt:
        """Execute full-trust Python, returning captured output or traceback.

        Args:
            code: Exact Python source approved for this WRITE call, max 65536 characters.
            expected_project_path: Absolute .uproject path that must match this editor.

        Returns:
            Receipt with actual execution status, stdout, stderr and traceback.
            No timeout or cancellation is promised for an executing script.
        """
        return _run_script(code, expected_project_path)
