//
// Created by Icyblazek on 2021/5/27.
// Github https://github.com/icyblazek
//
#include <cstdio>
#include <uv.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "args.hxx"
#include <spdlog/spdlog.h>


#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "IPHLPAPI.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "uv.lib")
#else

#endif // _WIN32

using namespace std;

string g_host = "localhost";
int g_debug_port = 1635;
string g_gateway = "eth-swarm.io";
string g_nodeId = "";
int g_gateway_port = 80;
bool g_auto_cashout = false;
int g_upload_interval = 1000 * 60;
bool g_auto_upload = true;
httplib::Client *g_httpClient = nullptr;
std::set<string> *g_tx_upload_queue = new set<string>();
std::set<string> *g_tx_uploaded_queue = new set<string>();

std::tuple<int, string> bee_health() {
    int res_code = -1;
    string version = "unknown";
    if (auto res = g_httpClient->Get("/health")) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("status") && json.at("status").get<string>() == "ok") {
                res_code = 0;
                version = json.at("version").get<string>();
            }
        }
    } else {
        res_code = static_cast<int>(res.error());
    }
    return std::tuple<int, string>(res_code, version);
}

int bee_peers() {
    int peerCount = 0;
    if (auto res = g_httpClient->Get("/peers")) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            peerCount = json.at("peers").size();
        }
    }
    return peerCount;
}

string bee_address() {
    string address;
    if (auto res = g_httpClient->Get("/addresses")) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("ethereum")) {
                address = json.at("ethereum").get<string>();
            }
        }
    }
    return address;
}

string bee_chequebook_address() {
    string address;
    if (auto res = g_httpClient->Get("/chequebook/address")) {
        if (res && res->status == 200) {
            /**
             * after bee 0.6.0,  chequebookaddress change to chequebookAddress
             */
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("chequebookaddress")) {
                address = json.at("chequebookaddress").get<string>();
            } else if (json.contains("chequebookAddress")) {
                address = json.at("chequebookAddress").get<string>();
            }
        }
    }
    return address;
}

std::tuple<int, std::list<nlohmann::json>> bee_lastcheques() {
    int res_code = -1;
    string body;
    std::list<nlohmann::json> peers;
    if (auto res = g_httpClient->Get("/chequebook/cheque")) {
        if (res && res->status == 200) {
            res_code = 0;
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("lastcheques")) {
                nlohmann::json::array_t lastcheques = json.at("lastcheques");
                for (auto &lastcheque : lastcheques) {
                    auto lastReceived = lastcheque.at("lastreceived");
                    if (lastReceived != nullptr) {
                        peers.push_back(lastcheque);
                    }
                }
            }
        }
    } else {
        res_code = static_cast<int>(res.error());
    }
    return std::tuple<int, std::list<nlohmann::json>>(res_code, peers);
}

double bee_get_cumulative_payout(const string &peer) {
    long payout = 0;

    if (auto res = g_httpClient->Get(("/chequebook/cheque/" + peer).c_str())) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("lastreceived")) {
                auto lastReceived = json.at("lastreceived");
                if (lastReceived != nullptr && lastReceived.contains("payout")) {
                    payout = lastReceived.at("payout").get<double>();
                }
            }
        }
    }
    return payout;
}

double bee_get_last_cashed_payout(const string &peer, double cashout_amount) {
    double payout = 0;

    if (auto res = g_httpClient->Get(("/chequebook/cashout/" + peer).c_str())) {
        if (res) {
            char tx[512];
            if (res->status == 200) {
                auto json = nlohmann::json::parse(res->body);
                if (json.contains("cumulativePayout")) {
                    payout = json.at("cumulativePayout").get<double>();
                    auto transactionHash = json.at("transactionHash").get<string>();

                    sprintf(tx, "%s,%s,%f", peer.c_str(), transactionHash.c_str(), payout);
                }
            } else if (res->status == 404) {
                sprintf(tx, "%s,%s,%f", peer.c_str(), "", cashout_amount);
            }
            string txStr(tx);

            auto iter = g_tx_uploaded_queue->find(txStr);
            if (iter == g_tx_uploaded_queue->end()) {
                g_tx_upload_queue->insert(txStr);
            }
        }
    }
    return payout;
}

double bee_get_uncashed_amount(const string &peer) {
    auto cumulativePayout = bee_get_cumulative_payout(peer);
    if (cumulativePayout <= 0) {
        return 0;
    }
    auto cashedPayout = bee_get_last_cashed_payout(peer, cumulativePayout);
    return cumulativePayout - cashedPayout;
}

bool bee_cashout(const string &peer, double uncashedAmount) {
    bool success = false;
    spdlog::info("uncashed cheque for {} ({} uncashed)", peer, uncashedAmount);
    if (auto res = g_httpClient->Post(("/chequebook/cashout/" + peer).c_str())) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("transactionHash")) {
                auto transactionHash = json.at("transactionHash").get<string>();
                if (transactionHash.length() > 0) {
                    success = true;
                    spdlog::info("cashing out cheque for {} in transaction {}", peer, transactionHash);

                    char tx[512];
                    sprintf(tx, "%s,%s,%f", peer.c_str(), transactionHash.c_str(), uncashedAmount);
                    string txStr(tx);
                    g_tx_upload_queue->insert(txStr);
                }
            } else {
                auto code = json.at("code").get<int>();
                auto message = json.at("message").get<string>();
                spdlog::error("cashout fail, code: {}, message: {}", code, message);
            }
        }
    }
    return success;
}

std::tuple<double, double> bee_get_balance() {
    if (auto res = g_httpClient->Get("/chequebook/balance")) {
        if (res && res->status == 200) {
            auto json = nlohmann::json::parse(res->body);
            double totalBalance = json.at("totalBalance").get<double>();
            double availableBalance = json.at("availableBalance").get<double>();
            return std::tuple<double, double>(totalBalance, availableBalance);
        }
    }
    return std::tuple<double, double>(0.0, 0.0);
}

void timer_cb(uv_timer_t *handle) {
    if (g_httpClient == nullptr) {
        uv_timer_stop(reinterpret_cast<uv_timer_t *>(&handle));
    }

    nlohmann::json data;
    auto health = bee_health();
    int result_code = std::get<0>(health);
    if (result_code == 0) {
        data["nodeId"] = g_nodeId;
        data["version"] = std::get<1>(health);
        data["status"] = "ok";
        data["address"] = bee_address();
        data["chequebookAddress"] = bee_chequebook_address();
        data["peersCount"] = bee_peers();
        data["debugPort"] = g_debug_port;

        auto lastcheques = bee_lastcheques();
        double totalAmount = 0;
        double uncashedAmount = 0;
        int availableTicket = 0;
        if (std::get<0>(lastcheques) == 0) {
            std::list<nlohmann::json> cheques = std::get<1>(lastcheques);
            for (auto &cheque : cheques) {
                auto lastReceived = cheque.at("lastreceived");
                availableTicket++;
                totalAmount += lastReceived.at("payout").get<double>();
                auto peer = cheque.at("peer").get<string>();
                auto t_uncashedAmount = bee_get_uncashed_amount(peer);
                if (t_uncashedAmount > 0 && g_auto_cashout) {
                    bee_cashout(peer, uncashedAmount);
                }
                uncashedAmount += t_uncashedAmount;
            }
        }
        auto balance = bee_get_balance();
        data["totalBalance"] = std::get<0>(balance);
        data["availableBalance"] = std::get<1>(balance);
        data["totalAmount"] = totalAmount;
        data["uncashedAmount"] = uncashedAmount;
        data["ticketCount"] = availableTicket;
    } else {
        data["status"] = "fail";
    }

    if (g_auto_upload) {
        httplib::Client client(g_gateway, g_gateway_port);
        auto postData = data.dump();
        auto res = client.Post("/agent/upload", postData, "application/json");
        if (res && res->status == 200) {
            spdlog::info("data upload success! {}", postData);
        } else {
            spdlog::info("data upload fail! {}", postData);
        }

        if (g_tx_upload_queue->size() > 0) {
            nlohmann::json tx(*g_tx_upload_queue);
            nlohmann::json txData;
            txData["nodeId"] = g_nodeId;
            txData["txs"] = tx;
            auto txUploadData = txData.dump();
            res = client.Post("/agent/tx_upload", txUploadData, "application/json");
            if (res && res->status == 200) {
                spdlog::info("tx data upload success! {}", txUploadData);
                g_tx_uploaded_queue->insert(g_tx_upload_queue->begin(), g_tx_upload_queue->end());
                g_tx_upload_queue->clear();
            } else {
                spdlog::info("tx data upload fail! {}", txUploadData);
            }
        }
    }
}

int main(int argc, char **argv) {
    args::ArgumentParser parser("swarm bee data agent!\nsource code: https://github.com/icyblazek/eth-swarm-agent",
                                "please visit http://eth-swarm.io");
    args::HelpFlag help(parser, "help", "display this help menu", {'h', "help"});
    args::ValueFlag<string> node_id(parser, "", "eth-swarm platform node id", {'n', "nid"});
    args::ValueFlag<string> host(parser, "", "default localhost", {"host"}, "localhost");
    args::ValueFlag<int> debug_port(parser, "", "default 1635", {'d'}, 1635);
    args::ValueFlag<string> gateway(parser, "", "default gateway: eth-swarm.io", {'g'}, "api.eth-swarm.io");
    args::ValueFlag<int> gateway_port(parser, "", "default gateway port: 80", {"gPort"}, 80);
    args::ValueFlag<bool> auto_cashout(parser, "", "auto cashout, default disable", {"auto"}, false);
    args::ValueFlag<int> upload_interval(parser, "min", "upload interval, default 5 min", {'t'}, 5);
    args::ValueFlag<bool> auto_upload(parser, "", "auto upload data, default enable", {"upload"}, true);
    try {
        parser.ParseCLI(argc, argv);
    } catch (const args::Help &) {
        std::cout << parser;
        return 0;
    } catch (const args::ParseError &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    if (!node_id) {
        spdlog::error("node_id can't be empty!!!");
        std::cerr << parser;
        return 1;
    }

    printf("@@@@@@@@-xxx-=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("@@@@@-xx      x-=@@@@@@@@@@===================-@@@@@=------------====-=@@@@@=================-@@@@@=----=@@@@@-----@@@@@\n");
    printf("@=-x    xxxxx    x-=@@@@@@@                   x@@@@@xxxxxxxxxxxxx     -@@@@= xxxxxxxxxxxxxxxx =@@@=        xxx       xxx\n");
    printf("x    xx--xxx--x     x@@@@@@=====@=x    -@@@@@@@@@@@@-xxxxxxxxxxx-x    =@@@@@@@@@@@@@@@@@@@@@@@@@@@xxxx-xxxx-xxxxx-xxx-@@\n");
    printf("    x-xx  xx=xx-x    @@@@@@@@@@@@=    x@@@@@@@@@@@@@-xxxxxxxxxxxx    x@@@@@@@@@@@@@@@@@@@@@@@@@@@@=xxxxxxxxxxxxxxxxxxx@@\n");
    printf("    x-x---x-=  x=x  x@@@@========-    x========@@@@xxxxxxxxxxxxxx    x@@@    xxxxxx           x@@@-    xxxxxxxxxx    -@@\n");
    printf("    --xx--x--  x-   x@@@@                     x@@@@------------xxxxx-=@@@----x     x-----------@@@x   x---------x    =@@\n");
    printf("    -x   x-=-  --   x@@@@========x    -=======@@@xxxxxxxxxxxxxxxxxxxxxx@@@@@=x    -@@@=----@@@@@@@x   xxxxxxxxxxx   x@@@\n");
    printf("     x--x  x----x    @@@@@@@@@@@=    x@@@@@@@@@@@-------------x    x---@@@@-    x=@@@@=    -@@@@@=xx    xxxxxxx    x-@@@\n");
    printf("x      x----xxx     x@@@@@@@@@@@-    x@@@@@@@@@@@@@@xxxxx@@@@@-    -@@@@@@x    x=@@@@@=x   x@@@@xxxx    xxxxxxx    xxx@@\n");
    printf("@=-x     xx      x-=@@@@@@@@@@@@x    -@@@@@@@@@@@@@@-    x@@@=x    =@@@@@x     xx           =@@@=-    x-=====x    x===@@\n");
    printf("@@@@=-x       x-=@@@@@@@@@@@@@@@-xxxx=@@@@@@@@@@@@@@@=====@@=xxxxx-@@@@@@--------=======xxxx-@@@@-xxxx=@@@@@@-xxxx=@@@@@\n");
    printf("@@@@@@@=-x x-=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("\n");

    g_host = args::get(host);
    g_debug_port = args::get(debug_port);
    g_gateway = args::get(gateway);
    g_gateway_port = args::get(gateway_port);
    g_auto_cashout = args::get(auto_cashout);
    g_upload_interval = args::get(upload_interval);
    g_auto_upload = args::get(auto_upload);
    g_nodeId = args::get(node_id);

    if (g_upload_interval < 1) {
        g_upload_interval = 60 * 1000;
        spdlog::warn("the upload interval should not be less than 1 minute!");
    } else {
        g_upload_interval = g_upload_interval * 1000 * 60;
    }

    printf("agent started! \nbee host: %s, debug port: %d, node_id: %s, gateway: %s:%d\n", g_host.c_str(), g_debug_port,
           args::get(node_id).c_str(), g_gateway.c_str(), g_gateway_port);

    g_httpClient = new httplib::Client(g_host, g_debug_port);
    g_httpClient->set_keep_alive(true);

    uv_loop_t *main_loop = uv_default_loop();

    uv_timer_s timer_req{};
    uv_timer_init(main_loop, &timer_req);
    // default 5 min repeat
    uv_timer_start(&timer_req, timer_cb, 5000, g_upload_interval);
    uv_run(main_loop, UV_RUN_DEFAULT);
    uv_stop(main_loop);
    uv_loop_close(main_loop);
    free(main_loop);
    return 0;
}