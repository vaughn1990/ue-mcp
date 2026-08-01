import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { editorTool } from "../../src/tools/editor.js";

describe("editor.invoke_function PIE targeting", () => {
  it("forwards an explicit PIE client selector to the native bridge", () => {
    const mapParams = editorTool.actions.invoke_function?.mapParams;
    expect(mapParams).toBeDefined();

    expect(mapParams!({
      actorLabel: "PlayerController_1",
      functionName: "ServerRequestGuildSnapshot",
      world: "pie",
      pieInstance: 1,
      worldPath: "/Game/UEDPIE_1_MMS_TestArena.MMS_TestArena",
    })).toEqual({
      actorLabel: "PlayerController_1",
      functionName: "ServerRequestGuildSnapshot",
      component: undefined,
      args: undefined,
      actorArgs: undefined,
      world: "pie",
      pieInstance: 1,
      worldPath: "/Game/UEDPIE_1_MMS_TestArena.MMS_TestArena",
    });
  });

  it("accepts only non-negative integer PIE instance selectors", () => {
    expect(editorTool.schema.pieInstance.safeParse(0).success).toBe(true);
    expect(editorTool.schema.pieInstance.safeParse(1).success).toBe(true);
    expect(editorTool.schema.pieInstance.safeParse(-1).success).toBe(false);
    expect(editorTool.schema.pieInstance.safeParse(1.5).success).toBe(false);
  });

  it("uses native PIE world-context selection and returns the selected target", () => {
    const source = readFileSync(resolve(
      "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/EditorHandlers_PIE.cpp",
    ), "utf8");
    const helpers = readFileSync(resolve(
      "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Public/HandlerUtils.h",
    ), "utf8");

    expect(source).toContain("FindPIEWorldContext(PieInstance, WorldPath)");
    expect(source).toContain("pieInstance and worldPath require world='pie'");
    expect(source).toContain('SetStringField(TEXT("worldPath"), World->GetPathName())');
    expect(source).toContain('SetNumberField(TEXT("pieInstance"), PieContext->PIEInstance)');
    expect(helpers).toContain("inline const FWorldContext* FindPIEWorldContext");
    expect(helpers).toContain("Context.PIEInstance != PieInstance");
    expect(source).toContain("Available PIE worlds");
  });
});
