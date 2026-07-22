// WebSocket Echo Server — demonstrates WebSocket support
//
// Usage: ./websocket_echo [port]
//
// Test with a browser or wscat:
//   npm install -g wscat
//   wscat -c ws://localhost:8080/ws
//   > Hello WebSocket!
//   < Hello WebSocket!
//
// Or with Python:
//   pip install websockets
//   python -c "
//   import asyncio, websockets
//   async def test():
//       async with websockets.connect('ws://localhost:8080/ws') as ws:
//           await ws.send('Hello!')
//           print(await ws.recv())
//   asyncio.run(test())
//   "

#include <async_net/http/server.hpp>
#include <async_net/http/websocket.hpp>
#include <async_net/io/io_context.hpp>
#include <async_net/coroutine/task.hpp>

#include <iostream>
#include <signal.h>

using namespace async_net;
using namespace async_net::http;

static bool g_running = true;
static void sig_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, sig_handler);

    io_context ctx;
    server srv(ctx, port);

    // Regular HTTP route — serves a simple HTML page with WebSocket client
    srv.route(method::GET, "/", [](const request&) -> Task<response> {
        std::string html = R"HTML(<!DOCTYPE html>
<html>
<head><title>WebSocket Echo</title></head>
<body>
<h1>WebSocket Echo Server</h1>
<input id="msg" type="text" placeholder="Type a message...">
<button onclick="send()">Send</button>
<div id="log"></div>
<script>
const ws = new WebSocket('ws://' + location.host + '/ws');
const log = document.getElementById('log');
ws.onopen = () => log.innerHTML += '<p style="color:green">Connected</p>';
ws.onmessage = (e) => log.innerHTML += '<p><b>Received:</b> ' + e.data + '</p>';
ws.onclose = () => log.innerHTML += '<p style="color:red">Disconnected</p>';
function send() {
    const input = document.getElementById('msg');
    ws.send(input.value);
    log.innerHTML += '<p><b>Sent:</b> ' + input.value + '</p>';
    input.value = '';
}
</script>
</body>
</html>)HTML";

        response resp;
        resp.status = status_code::ok();
        resp.hdrs.set("Content-Type", "text/html");
        resp.hdrs.set("Content-Length", std::to_string(html.size()));
        resp.bd = body(html);
        co_return resp;
    });

    // WebSocket echo route
    srv.ws_route("/ws", [](ws::websocket_connection& ws) -> Task<void> {
        std::cout << "[ws] New WebSocket connection" << std::endl;

        while (true) {
            auto msg = co_await ws.receive();
            if (msg.empty()) {
                std::cout << "[ws] Connection closed" << std::endl;
                break;
            }

            std::cout << "[ws] Received: " << msg << std::endl;

            // Echo back
            bool ok = co_await ws.send(msg);
            if (!ok) {
                std::cout << "[ws] Send failed" << std::endl;
                break;
            }
        }

        co_return;
    });

    std::cout << "WebSocket Echo Server on http://localhost:" << port << std::endl;
    std::cout << "Open http://localhost:" << port << "/ in a browser" << std::endl;

    // Run server
    auto task = srv.serve();
    task.resume();

    while (g_running) {
        ctx.poll();
    }

    srv.stop();
    return 0;
}
