#pragma once

#define SEND_MESSAGE(header, msg)                                              \
    sendHeader(header);                                                        \
    size_t msgSize = msg.ByteSizeLong();                                       \
    {                                                                          \
        uint8_t sMsg[msgSize];                                                 \
        if (!msg.SerializeToArray(sMsg, msgSize)) {                            \
            throw std::runtime_error("Error serialising message");             \
        }                                                                      \
        send(sMsg, msgSize);                                                   \
    }

#define SEND_MESSAGE_PTR(header, msg)                                          \
    sendHeader(header);                                                        \
    size_t msgSize = msg->ByteSizeLong();                                      \
    {                                                                          \
        uint8_t sMsg[msgSize];                                                 \
        if (!msg->SerializeToArray(sMsg, msgSize)) {                           \
            throw std::runtime_error("Error serialising message");             \
        }                                                                      \
        send(sMsg, msgSize);                                                   \
    }

#define SEND_SERVER_RESPONSE(msg, host, port)                                  \
    size_t msgSize = msg.ByteSizeLong();                                       \
    {                                                                          \
        uint8_t sMsg[msgSize];                                                 \
        if (!msg.SerializeToArray(sMsg, msgSize)) {                            \
            throw std::runtime_error("Error serialising message");             \
        }                                                                      \
        sendResponse(sMsg, msgSize, host, port);                               \
    }

#define PARSE_MSG(T, data, size)                                               \
    T msg;                                                                     \
    if (!msg.ParseFromArray(data, size)) {                                     \
        throw std::runtime_error("Error deserialising message");               \
    }

#define PARSE_RESPONSE(T, data, size)                                          \
    T response;                                                                \
    if (!response.ParseFromArray(data, size)) {                                \
        throw std::runtime_error("Error deserialising message");               \
    }