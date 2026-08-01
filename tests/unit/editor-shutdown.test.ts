import { once } from "node:events";
import { readFileSync } from "node:fs";
import type { AddressInfo } from "node:net";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";
import { describe, expect, it } from "vitest";

import { requestEditorSelfClose } from "../../src/editor-control.js";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..");

describe("safe editor shutdown", () => {
  it("requests the dedicated native close handler instead of execute_python", async () => {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(server, "listening");
    const port = (server.address() as AddressInfo).port;
    let request: Record<string, unknown> | undefined;

    server.on("connection", (socket) => {
      socket.on("message", (data) => {
        request = JSON.parse(data.toString()) as Record<string, unknown>;
        socket.send(JSON.stringify({ id: request.id, result: { success: true } }));
      });
    });

    try {
      await expect(requestEditorSelfClose(port)).resolves.toBe(true);
      expect(request).toMatchObject({
        id: "ue-mcp-stop",
        method: "request_editor_close",
        params: {},
      });
      expect(JSON.stringify(request)).not.toContain("execute_python");
      expect(JSON.stringify(request)).not.toContain("quit_editor");
    } finally {
      for (const client of server.clients) client.terminate();
      await new Promise<void>((resolve, reject) => {
        server.close((error) => (error ? reject(error) : resolve()));
      });
    }
  });

  it("uses Unreal's deferred MainFrame close and joins connection workers", () => {
    const handlers = readFileSync(
      join(root, "plugin", "ue_mcp_bridge", "Source", "UE_MCP_Bridge", "Private", "Handlers", "EditorHandlers.cpp"),
      "utf8",
    );
    const server = readFileSync(
      join(root, "plugin", "ue_mcp_bridge", "Source", "UE_MCP_Bridge", "Private", "BridgeServer.cpp"),
      "utf8",
    );

    const nativeClose = handlers.slice(
      handlers.indexOf("FEditorHandlers::RequestEditorClose"),
      handlers.indexOf("FEditorHandlers::ExecuteCommand"),
    );
    expect(nativeClose).toContain('DeferredCommands.AddUnique(TEXT("CLOSE_SLATE_MAINFRAME"))');
    expect(nativeClose).toContain("static bool bCloseScheduled = false");
		expect(nativeClose).toContain("bCloseScheduled = false;");
    expect(nativeClose).not.toContain('Exec(nullptr, TEXT("QUIT_EDITOR")');
    expect(nativeClose).not.toContain("UKismetSystemLibrary::QuitEditor");
    expect(nativeClose).not.toContain("CloseAllAssetEditors");
    expect(server).toContain("while (ActiveConnectionTasks.GetValue() > 0)");
    expect(server).toContain("ActiveConnectionTasks.Decrement()");
  });

  it("rejects a bridge error instead of reporting the close as delivered", async () => {
    const server = new WebSocketServer({ host: "127.0.0.1", port: 0 });
    await once(server, "listening");
    const port = (server.address() as AddressInfo).port;
    server.on("connection", (socket) => {
      socket.on("message", () => {
        socket.send(JSON.stringify({ id: "ue-mcp-stop", error: { code: -1, message: "not ready" } }));
      });
    });

    try {
      await expect(requestEditorSelfClose(port)).resolves.toBe(false);
    } finally {
      for (const client of server.clients) client.terminate();
      await new Promise<void>((resolve, reject) => {
        server.close((error) => (error ? reject(error) : resolve()));
      });
    }
  });
});
