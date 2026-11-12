#pragma once

// McpServer - Embedded MCP (Model Context Protocol) server
// Runs an HTTP+SSE server on localhost for AI agent integration.

namespace McpServer {
    // Start the MCP HTTP server on a background thread.
    // port: TCP port to listen on (default 21337)
    void Init(int port = 21337);

    // Stop the server and wait for the thread to exit.
    void Shutdown();

    // Process teardown only: detach the C++ thread object under loader lock.
    // Normal shutdown must instead call Shutdown() outside loader lock.
    void AbandonOnProcessExit();

    // Returns true if the server is listening.
    bool IsRunning();

    // Returns the port the server is listening on.
    int GetPort();

    // Returns number of MCP requests handled since startup.
    int GetRequestCount();
}
