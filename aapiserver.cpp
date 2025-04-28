#include <iostream>
#include <string>
#include <csignal>
#include <vector>
#include <utility>
#include <fstream>

#include "windowsHeader.hpp"
#include "libsocket.hpp"
#include "libhttp.hpp"
#include "libjson.hpp"
#include "libpoll.hpp"
#include "liburlcode.hpp"
using namespace std;
using namespace http;

Json json;
Poller poller;

bool exit_flag = false;
bool enable_aapi_namelist = false;

string API_HOST = "jaam.net.ua";
string API_URI = "/alerts_statuses_v1.json";

vector<pair<string, string>> AAPI_NAMELIST;

void AAPILog(string module, string str) {
    cout << "[AAPILOG][" + module +  "] " + str << endl;
}

void AAPILogSocket(Socket client, HTTPRequest req) {
    AAPILog(client.remoteSocketAddress().str(), implMethods[req.getMethod()] + " " + uriDecode(req.getURI())); 
}

pair<string, string> findNamePair(string name) {
    if (enable_aapi_namelist) for (auto i : AAPI_NAMELIST) if (i.first == name || i.second == name) return i;

    return {name, name};
}

JsonNode request_JAAM_API() {
    Socket jaam(AF_INET, SOCK_STREAM);
    jaam.connect(API_HOST, 80);

    HTTPRequest request;
    request.setMethod(HTTP_GET);
    request.setURI(API_URI);
    request.setVersion("HTTP/1.1");
    request.addHeader("Host", API_HOST);
    request.addHeader("User-Agent", "AAPI/1.0");
    request.addHeader("Connection", "close");

    jaam.send(dump_http_request(request));

    HTTPResponse response = parse_http_response(jaam.recv(65536).buffer.toString());

    string body = response.getBody();
    size_t bodySize = stoul(response.getHeader("Content-Length")[0].value1);
    
    while (body.size() < bodySize) body += jaam.recvall(bodySize - body.size()).buffer.toString();

    return json.parse(body);
}

void loadAAPINamelist(string path) {
    ifstream namefile(path);
    if (!namefile.good()) {
        AAPILog("loadAAPINamelist", "Loading error! File " + path + " not found.");
        return;
    }

    JsonNode names = json.parse(namefile);

    for (auto i : names.array) AAPI_NAMELIST.push_back({i[0].str, i[1].str});

    enable_aapi_namelist = true;
    AAPILog("loadAAPINamelist", "Loading successful!");
}

void newClient(Socket server) {
    Socket newSocket = server.accept();
    newSocket.setblocking(false);

    poller.addDescriptor(newSocket, POLLIN);
}

void closeClient(Socket client) {
    poller.removeDescriptor(client);
    client.close();
}

void handleClient(Socket client) {
    SocketData data = client.recv(8192);
    HTTPRequest req = parse_http_request(data.buffer.toString());
    
    AAPILogSocket(client, req);

    JsonNode jaam_api_data = request_JAAM_API();
    JsonNode api_data;

    HTTPResponse resp;
    resp.setCode(200);
    resp.setVersion("HTTP/1.1");
    resp.addHeader("Content-Type", "application/json; charset=utf-8");
    resp.addHeader("Connection", "keep-alive");
    resp.addHeader("Keep-Alive", "timeout=5, max=10");
    resp.addHeader("Server", "AAPI/1.0");

    string uri = uriDecode(req.getURI());
    

    if (uri == "/") {
        for (auto [key, value] : jaam_api_data["states"].objects) {
            JsonNode tmp;
            tmp.addPair("alert", value["enabled"]);
            tmp.addPair("start", value["enabled_at"]);
            tmp.addPair("stop", value["disabled_at"]);

            api_data.addPair(findNamePair(key).second, tmp);
        }
    } else {
        uri.erase(0, 1);

        pair<string, string> names = findNamePair(uri);
        string jaam_name = names.first;
        string aapi_name = names.second;

        JsonNode tmp = jaam_api_data["states"][jaam_name];

        if (tmp.is_null()) {
            resp.setCode(404);
            api_data.addPair("Error", "Not found");
        } else {
            api_data.addPair("alert", tmp["enabled"]);
            api_data.addPair("start", tmp["enabled_at"]);
            api_data.addPair("stop", tmp["disabled_at"]);
        }
    }

    resp.setBody(json.dump(api_data));
    resp.addHeader("Content-Length", to_string(resp.getBody().size()));
    

    client.sendall(dump_http_response(resp));
}

int main() {
    AAPILog("Main", "Starting Alert API Server");

    AAPILog("loadAAPINamelist", "Loading namelist...");
    loadAAPINamelist("aapi_names.json");

    signal(SIGINT, [](int e){
        AAPILog("Main", "Received SIGINT signal, setting exit flag.");
        exit_flag = true;
    });

    Socket server(AF_INET, SOCK_STREAM);
    server.setreuseaddr(true);
    server.bind("", 19751);
    server.listen(0);

    AAPILog("Server", "Server running on address: " + server.localSocketAddress().str());

    poller.addDescriptor(server, POLLIN);

    while (!exit_flag) {
        pollEvents events = poller.poll(10);

        if (!events.size()) continue;

        for (auto ev : events) {

            Socket eventSocket = ev.fd;

            if (ev.event & POLLIN) {
                if (eventSocket == server) newClient(server);

                else {
                    if (!eventSocket.tcpRecvAvailable()) closeClient(eventSocket);
                    else handleClient(eventSocket);
                }
            }
            
            else if (ev.event & POLLHUP) closeClient(eventSocket);
        }
    }

    AAPILog("Server", "Detected exit flag, closing sockets...");
    poller.removeDescriptor(server);

    for (auto i : poller.getDescriptors()) Socket(i.fd).close();
    poller.removeAllDescriptors();

    AAPILog("Server", "Shutting down...");
    server.close();

    AAPILog("Main", "Exiting...");
}