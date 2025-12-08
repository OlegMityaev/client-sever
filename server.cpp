// server.cpp
// Серверная часть приложения: приём графов от клиентов и вычисление кратчайших путей.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "graph.hpp"
#include "protocol.hpp"

namespace {

constexpr int kListenBacklog = 16;

enum class Transport { Tcp, Udp };

struct ClientContext {
    graph::GraphDefinition graph;
    bool hasGraph = false;
};

struct UdpClientKey {
    std::string host;
    uint16_t port = 0;

    bool operator==(const UdpClientKey& other) const {
        return host == other.host && port == other.port;
    }
};

struct UdpClientKeyHasher {
    std::size_t operator()(const UdpClientKey& key) const noexcept {
        return std::hash<std::string>()(key.host) ^ (std::hash<uint16_t>()(key.port) << 1);
    }
};

// Построение текста справки: возвращает строку с описанием доступных команд сервера.
std::string buildHelpText() {
    return "Команды:\n"
           "  help            - получить список команд\n"
           "  upload_graph    - загрузить граф (матрица инцидентности + веса)\n"
           "  path_query      - найти кратчайший путь между вершинами\n"
           "  exit            - завершить соединение клиента\n"
           "Нумерация вершин начинается с 0.\n";
}

// Создание полезной нагрузки ошибки: формирует сообщение об ошибке и устанавливает соответствующий заголовок.
std::vector<uint8_t> makeErrorPayload(const std::string& message,
                                      netproto::MessageHeader& header) {
    header.command = netproto::Command::Error;
    header.status = netproto::Status::InvalidRequest;
    return netproto::serializeString(message);
}

// Создание полезной нагрузки со строкой (для help): формирует ответ со строкой и устанавливает заголовок Help.
std::vector<uint8_t> makeOkStringPayload(const std::string& message,
                                         netproto::MessageHeader& header) {
    header.command = netproto::Command::Help;
    header.status = netproto::Status::Ok;
    return netproto::serializeString(message);
}

// Декодирование полезной нагрузки графа: десериализует граф из бинарного формата и выполняет валидацию.
// Возвращает nullopt при ошибке десериализации или валидации, записывая описание в errorMessage.
std::optional<graph::GraphDefinition> decodeGraphPayload(
    const std::vector<uint8_t>& payload,
    std::string& errorMessage) {
    netproto::UploadGraphPayload encoded;
    if (!netproto::deserializeUploadGraph(payload, encoded, errorMessage)) {
        return std::nullopt;
    }
    graph::GraphDefinition definition;
    definition.vertexCount = encoded.vertexCount;
    definition.edgeCount = encoded.edgeCount;
    definition.weights = encoded.weights;
    if (!netproto::unpackIncidenceMatrix(encoded.vertexCount,
                                         encoded.edgeCount,
                                         encoded.incidenceBits,
                                         definition.incidence,
                                         errorMessage)) {
        return std::nullopt;
    }
    graph::ValidationResult validation = graph::validateGraph(definition);
    if (!validation.ok) {
        errorMessage = validation.message;
        return std::nullopt;
    }
    return std::make_optional(definition);
}

// Построение полезной нагрузки результата пути: формирует ответ с результатом поиска пути.
// Если путь не найден, возвращает сообщение об ошибке. Иначе возвращает PathResult с длиной и маршрутом.
std::vector<uint8_t> buildPathResultPayload(const graph::PathComputation& result,
                                            netproto::MessageHeader& header) {
    if (!result.reachable) {
        header.command = netproto::Command::Error;
        header.status = netproto::Status::NotReady;
        return netproto::serializeString(result.error.empty()
                                             ? "Путь не найден."
                                             : result.error);
    }
    header.command = netproto::Command::PathResult;
    header.status = netproto::Status::Ok;
    netproto::PathResultPayload payload;
    payload.distance = result.distance;
    payload.path = result.path;
    return netproto::serializePathResult(payload);
}

// Приём точного количества байтов через TCP-сокет: гарантирует получение всех запрошенных байтов.
// Выполняет повторные вызовы recv() до тех пор, пока не будет получено нужное количество байтов.
bool recvExact(int socket, uint8_t* buffer, size_t size) {
    size_t received = 0;
    while (received < size) {
        ssize_t chunk = recv(socket,
                             reinterpret_cast<char*>(buffer) + received,
                             static_cast<int>(size - received),
                             0);
        if (chunk <= 0) {
            return false;
        }
        received += static_cast<size_t>(chunk);
    }
    return true;
}

// Отправка всех данных через TCP-сокет: гарантирует отправку всех байтов, даже если send() отправляет частично.
// Выполняет повторные вызовы send() до тех пор, пока все данные не будут отправлены.
bool sendAll(int socket, const uint8_t* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t chunk = send(socket,
                             reinterpret_cast<const char*>(data) + sent,
                             static_cast<int>(size - sent),
                             0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

// Отправка TCP-сообщения: отправляет заголовок и полезную нагрузку через TCP-сокет.
// Устанавливает payloadSize в заголовке перед отправкой.
bool sendTcpMessage(int socket,
                    netproto::MessageHeader header,
                    const std::vector<uint8_t>& payload) {
    header.payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> headerBuf = netproto::serializeHeader(header);
    if (!sendAll(socket, headerBuf.data(), headerBuf.size())) {
        return false;
    }
    if (!payload.empty()) {
        return sendAll(socket, payload.data(), payload.size());
    }
    return true;
}

// Чтение TCP-сообщения: получает заголовок и полезную нагрузку из TCP-сокета.
// Сначала читает заголовок фиксированного размера, затем полезную нагрузку указанного размера.
bool readTcpMessage(int socket, netproto::MessageHeader& header, std::vector<uint8_t>& payload) {
    std::vector<uint8_t> headerBuf(netproto::kHeaderSize);
    if (!recvExact(socket, headerBuf.data(), headerBuf.size())) {
        return false;
    }
    if (!netproto::deserializeHeader(headerBuf, header)) {
        return false;
    }
    payload.resize(header.payloadSize);
    if (header.payloadSize > 0 &&
        !recvExact(socket, payload.data(), payload.size())) {
        return false;
    }
    return true;
}

// Создание заголовка сообщения: формирует заголовок с указанными параметрами команды, статуса и requestId.
netproto::MessageHeader makeHeader(netproto::Command cmd, netproto::Status status, uint16_t requestId) {
    netproto::MessageHeader header;
    header.command = cmd;
    header.status = status;
    header.requestId = requestId;
    header.payloadSize = 0;
    header.reserved = 0;
    return header;
}

// Обработка TCP-клиента: функция, выполняемая в отдельном потоке для каждого подключённого клиента.
// Читает запросы от клиента, обрабатывает команды (Help, UploadGraph, PathQuery, Exit) и отправляет ответы.
// Хранит состояние графа для данного клиента в локальной переменной context.
void handleTcpClient(int clientSocket, sockaddr_in clientAddr) {
    ClientContext context;
    char addrBuf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, addrBuf, sizeof(addrBuf));
    std::cout << "TCP клиент подключен: " << addrBuf << ":" << ntohs(clientAddr.sin_port) << "\n";

    while (true) {
        netproto::MessageHeader requestHeader;
        std::vector<uint8_t> payload;
        if (!readTcpMessage(clientSocket, requestHeader, payload)) {
            std::cout << "Соединение с клиентом завершено.\n";
            break;
        }

        netproto::MessageHeader responseHeader = makeHeader(netproto::Command::Error,
                                                            netproto::Status::InvalidRequest,
                                                            requestHeader.requestId);
        std::vector<uint8_t> responsePayload;

        switch (requestHeader.command) {
            case netproto::Command::Help: {
                responsePayload = makeOkStringPayload(buildHelpText(), responseHeader);
                break;
            }
            case netproto::Command::UploadGraph: {
                std::string error;
                auto graphDefinition = decodeGraphPayload(payload, error);
                if (!graphDefinition) {
                    responsePayload = makeErrorPayload(error, responseHeader);
                } else {
                    context.graph = *graphDefinition;
                    context.hasGraph = true;
                    responseHeader.command = netproto::Command::UploadGraph;
                    responseHeader.status = netproto::Status::Ok;
                    responsePayload = netproto::serializeString("Граф принят сервером.");
                }
                break;
            }
            case netproto::Command::PathQuery: {
                netproto::PathQueryPayload query{};
                if (!netproto::deserializePathQuery(payload, query)) {
                    responsePayload = makeErrorPayload("Некорректная структура PathQuery.", responseHeader);
                    break;
                }
                if (!context.hasGraph) {
                    responsePayload = makeErrorPayload("Граф не загружен. Используйте upload_graph.", responseHeader);
                    break;
                }
                graph::PathComputation computation = graph::bellmanFord(context.graph,
                                                                        query.source,
                                                                        query.target);
                responsePayload = buildPathResultPayload(computation, responseHeader);
                break;
            }
            case netproto::Command::Exit: {
                responseHeader.command = netproto::Command::Exit;
                responseHeader.status = netproto::Status::Ok;
                responsePayload = netproto::serializeString("До свидания.");
                sendTcpMessage(clientSocket, responseHeader, responsePayload);
                std::cout << "Клиент инициировал завершение соединения.\n";
                close(clientSocket);
                return;
            }
            default: {
                responsePayload = makeErrorPayload("Неизвестная команда.", responseHeader);
                break;
            }
        }

        if (!sendTcpMessage(clientSocket, responseHeader, responsePayload)) {
            std::cout << "Ошибка отправки ответа клиенту.\n";
            break;
        }
    }

    close(clientSocket);
}

// Отправка UDP-сообщения: отправляет заголовок и полезную нагрузку через UDP-сокет указанному адресу.
// Устанавливает payloadSize в заголовке перед отправкой.
bool sendUdpMessage(int socket,
                    const sockaddr_in& clientAddr,
                    netproto::MessageHeader header,
                    const std::vector<uint8_t>& payload) {
    header.payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> headerBuf = netproto::serializeHeader(header);
    std::vector<uint8_t> packet;
    packet.reserve(headerBuf.size() + payload.size());
    packet.insert(packet.end(), headerBuf.begin(), headerBuf.end());
    packet.insert(packet.end(), payload.begin(), payload.end());

    ssize_t sent = sendto(socket,
                          packet.data(),
                          static_cast<int>(packet.size()),
                          0,
                          reinterpret_cast<const sockaddr*>(&clientAddr),
                          sizeof(clientAddr));
    return sent == static_cast<ssize_t>(packet.size());
}

// Отправка UDP-подтверждения: отправляет ACK клиенту с указанным requestId для подтверждения получения сообщения.
void sendUdpAck(int socket, const sockaddr_in& clientAddr, uint16_t requestId) {
    netproto::MessageHeader ack = makeHeader(netproto::Command::Ack,
                                             netproto::Status::Ok,
                                             requestId);
    sendUdpMessage(socket, clientAddr, ack, {});
}

// Преобразование адреса в строковый ключ: создаёт уникальный ключ для идентификации UDP-клиента.
// Формат: "IP:порт". Используется для хранения состояния графа каждого клиента.
std::string addrToKey(const sockaddr_in& addr) {
    char host[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
    std::ostringstream key;
    key << host << ":" << ntohs(addr.sin_port);
    return key.str();
}

// Запуск TCP-сервера: создаёт TCP-сокет, привязывает его к порту и начинает прослушивание.
// Для каждого подключённого клиента создаёт отдельный поток, который обрабатывает запросы клиента.
// Сервер работает до завершения процесса (по сигналу от пользователя).
void runTcpServer(uint16_t port) {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        perror("socket");
        return;
    }
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        perror("bind");
        close(serverSocket);
        return;
    }
    if (listen(serverSocket, kListenBacklog) < 0) {
        perror("listen");
        close(serverSocket);
        return;
    }
    std::cout << "TCP сервер слушает порт " << port << "\n";

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientSocket = accept(serverSocket,
                                  reinterpret_cast<sockaddr*>(&clientAddr),
                                  &addrLen);
        if (clientSocket < 0) {
            perror("accept");
            continue;
        }
        std::thread worker(handleTcpClient, clientSocket, clientAddr);
        worker.detach();
    }
}

// Запуск UDP-сервера: создаёт UDP-сокет, привязывает его к порту и начинает обработку датаграмм.
// Хранит состояние графа для каждого клиента в хеш-таблице (ключ - адрес клиента).
// Для каждого входящего пакета отправляет ACK, обрабатывает команду и отправляет ответ.
// Сервер работает в одном потоке, обрабатывая запросы последовательно.
void runUdpServer(uint16_t port) {
    int serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0) {
        perror("socket");
        return;
    }
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if (bind(serverSocket,
             reinterpret_cast<sockaddr*>(&serverAddr),
             sizeof(serverAddr)) < 0) {
        perror("bind");
        close(serverSocket);
        return;
    }
    std::cout << "UDP сервер слушает порт " << port << "\n";

    std::unordered_map<std::string, ClientContext> clients;
    std::mutex clientsMutex;

    while (true) {
        std::vector<uint8_t> buffer(65536);
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        ssize_t bytes = recvfrom(serverSocket,
                                 buffer.data(),
                                 static_cast<int>(buffer.size()),
                                 0,
                                 reinterpret_cast<sockaddr*>(&clientAddr),
                                 &clientLen);
        if (bytes < 0) {
            perror("recvfrom");
            continue;
        }
        if (bytes < static_cast<ssize_t>(netproto::kHeaderSize)) {
            std::cout << "От клиента получен слишком короткий пакет.\n";
            continue;
        }
        buffer.resize(static_cast<size_t>(bytes));
        std::vector<uint8_t> headerBuf(buffer.begin(),
                                       buffer.begin() + netproto::kHeaderSize);
        netproto::MessageHeader requestHeader;
        if (!netproto::deserializeHeader(headerBuf, requestHeader)) {
            std::cout << "Не удалось разобрать заголовок UDP-пакета.\n";
            continue;
        }
        std::vector<uint8_t> payload(buffer.begin() + netproto::kHeaderSize,
                                     buffer.end());

        sendUdpAck(serverSocket, clientAddr, requestHeader.requestId);

        ClientContext* context = nullptr;
        std::string key = addrToKey(clientAddr);
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            context = &clients[key];
        }

        netproto::MessageHeader responseHeader = makeHeader(netproto::Command::Error,
                                                            netproto::Status::InvalidRequest,
                                                            requestHeader.requestId);
        std::vector<uint8_t> responsePayload;

        switch (requestHeader.command) {
            case netproto::Command::Help: {
                responsePayload = makeOkStringPayload(buildHelpText(), responseHeader);
                break;
            }
            case netproto::Command::UploadGraph: {
                std::string error;
                auto graphDefinition = decodeGraphPayload(payload, error);
                if (!graphDefinition) {
                    responsePayload = makeErrorPayload(error, responseHeader);
                } else {
                    context->graph = *graphDefinition;
                    context->hasGraph = true;
                    responseHeader.command = netproto::Command::UploadGraph;
                    responseHeader.status = netproto::Status::Ok;
                    responsePayload = netproto::serializeString("Граф принят сервером.");
                }
                break;
            }
            case netproto::Command::PathQuery: {
                netproto::PathQueryPayload query{};
                if (!netproto::deserializePathQuery(payload, query)) {
                    responsePayload = makeErrorPayload("Некорректная структура PathQuery.", responseHeader);
                    break;
                }
                if (!context->hasGraph) {
                    responsePayload = makeErrorPayload("Граф не загружен. Используйте load_graph.", responseHeader);
                    break;
                }
                graph::PathComputation computation = graph::bellmanFord(context->graph,
                                                                        query.source,
                                                                        query.target);
                responsePayload = buildPathResultPayload(computation, responseHeader);
                break;
            }
            case netproto::Command::Exit: {
                responseHeader.command = netproto::Command::Exit;
                responseHeader.status = netproto::Status::Ok;
                responsePayload = netproto::serializeString("До свидания.");
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    clients.erase(key);
                }
                break;
            }
            default: {
                responsePayload = makeErrorPayload("Неизвестная команда.", responseHeader);
                break;
            }
        }

        if (!sendUdpMessage(serverSocket, clientAddr, responseHeader, responsePayload)) {
            std::cout << "Не удалось отправить ответ UDP-клиенту.\n";
        }
    }
}

// Парсинг протокола: преобразует строку "tcp" или "udp" в значение enum Transport.
// Возвращает nullopt для неизвестного протокола.
std::optional<Transport> parseTransport(const std::string& protocol) {
    if (protocol == "tcp") {
        return Transport::Tcp;
    }
    if (protocol == "udp") {
        return Transport::Udp;
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Использование: " << argv[0] << " <protocol> <port>\n";
        return 1;
    }
    std::string protocol = argv[1];
    auto transportOpt = parseTransport(protocol);
    if (!transportOpt) {
        std::cerr << "Неизвестный протокол. Используйте tcp или udp.\n";
        return 1;
    }
    int port = std::stoi(argv[2]);
    if (port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
        std::cerr << "Некорректный номер порта.\n";
        return 1;
    }
    uint16_t portValue = static_cast<uint16_t>(port);

    if (*transportOpt == Transport::Tcp) {
        runTcpServer(portValue);
    } else {
        runUdpServer(portValue);
    }

    return 0;
}


