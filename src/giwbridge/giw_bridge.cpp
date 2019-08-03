/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2017 GDB ImageWatch contributors
 * (github.com/csantosbh/gdb-imagewatch/)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <string>
#include <deque>
#include <iostream>

#include <signal.h>

#include "giw_bridge.h"
#include "debuggerinterface/buffer_request_message.h" // TODO move py_copy_string to python_native_interface
#include "debuggerinterface/preprocessor_directives.h"
#include "debuggerinterface/python_native_interface.h"
#include "ipc/message_exchange.h"

#include <QProcess>
#include <QTcpServer>
#include <QDataStream>
#include <QTcpSocket>
#include <QString>

using namespace std;

struct UiMessage {
    virtual ~UiMessage() {}
};

struct GetObservedSymbolsResponseMessage : public UiMessage {
    std::deque<string> observed_symbols;
};

struct PlotBufferRequestMessage : public UiMessage {
    std::string buffer_name;
};

class GiwBridge
{
public:
    GiwBridge(int(*plot_callback)(const char*))
        : client_(nullptr)
        , plot_callback_(plot_callback) {}

    bool start()
    {
        // Initialize server
        const uint16_t host_port = 9588; // TODO parameterize
        if(!server_.listen(QHostAddress::Any, host_port)) {
            // TODO escalate error
            cerr << "[giw] Could not start TCP server" << endl;
            return false;
        }

        // TODO get proper binary path at runtime
        QString program = "/Users/claudio.fernandes/workspace/pessoal/gdb-imagewatch/build/src/giwwindow.app/Contents/MacOS/giwwindow";
        QStringList arguments;
        arguments << "-style" << "fusion";
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start(program, arguments);

        wait_for_client();

        return client_ != nullptr;
    }

    bool is_window_ready()
    {
        return client_ != nullptr &&
               process_.processId() != 0 &&
               kill(process_.processId(), 0) == 0;
    }

    deque<string> get_observed_symbols()
    {
        assert(client_ != nullptr);

        MessageComposer message_composer;
        message_composer.push(MessageType::GetObservedSymbols);
        message_composer.send(client_);

        auto response = fetch_message(MessageType::GetObservedSymbolsResponse);
        if(response != nullptr) {
            return static_cast<GetObservedSymbolsResponseMessage*>(response.get())->observed_symbols;
        } else {
            return {};
        }
    }


    void set_available_symbols(const deque<string>& available_vars)
    {
        assert(client_ != nullptr);

        MessageComposer message_composer;
        message_composer.push(MessageType::SetAvailableSymbols);
        message_composer.push(available_vars);
        message_composer.send(client_);
    }

    void run_event_loop()
    {
        try_read_incoming_messages(static_cast<int>(1000.0 / 5.0));

        unique_ptr<UiMessage> plot_request_message;
        while((plot_request_message = try_get_stored_message(
                   MessageType::PlotBufferRequest)) != nullptr) {
            const PlotBufferRequestMessage* msg =
                    dynamic_cast<PlotBufferRequestMessage*>(
                        plot_request_message.get());
            plot_callback_(msg->buffer_name.c_str());
        }
    }

    ~GiwBridge() {
        process_.kill();
    }

private:
    QProcess process_;
    QTcpServer server_;
    QTcpSocket* client_;

    int (*plot_callback_)(const char*);

    std::map<MessageType, std::unique_ptr<UiMessage>> received_messages_;

    std::unique_ptr<UiMessage> try_get_stored_message(const MessageType& msg_type) {
        auto find_msg_handler = received_messages_.find(msg_type);

        if(find_msg_handler != received_messages_.end()) {
            unique_ptr<UiMessage> result = std::move(find_msg_handler->second);
            received_messages_.erase(find_msg_handler);
            return result;
        }

        return nullptr;
    }


    void try_read_incoming_messages(int msecs = 3000) {
        assert(client_ != nullptr);

        do {
            client_->waitForReadyRead(msecs);

            if (client_->bytesAvailable() == 0) {
                break;
            }

            MessageType header;
            client_->read(reinterpret_cast<char*>(&header),
                          static_cast<qint64>(sizeof(header)));

            switch (header) {
            case MessageType::PlotBufferRequest:
                received_messages_[header] = decode_plot_buffer_request();
                break;
            case MessageType::GetObservedSymbolsResponse:
                received_messages_[header] = decode_get_observed_symbols_response();
                break;
            default:
                cerr << "[giw] Received message with incorrect header" << endl;
                break;
            }
        } while(client_->bytesAvailable() > 0);
    }


    unique_ptr<UiMessage> decode_plot_buffer_request()
    {
        assert(client_ != nullptr);

        auto response = new PlotBufferRequestMessage();
        MessageDecoder::receive_string(client_, response->buffer_name);
        return unique_ptr<UiMessage>(response);
    }

    unique_ptr<UiMessage> decode_get_observed_symbols_response()
    {
        assert(client_ != nullptr);

        auto response = new GetObservedSymbolsResponseMessage();

        MessageDecoder::receive_symbol_list<std::deque<std::string>,
                std::string>(client_, response->observed_symbols);

        return unique_ptr<UiMessage>(response);
    }

    std::unique_ptr<UiMessage> fetch_message(const MessageType& msg_type) {
        // Return message if it was already received before
        auto result = try_get_stored_message(msg_type);

        if(result != nullptr) {
            return result;
        }

        // Try to fetch message
        try_read_incoming_messages();

        return try_get_stored_message(msg_type);
    }


    void wait_for_client() {
        if(client_ == nullptr) {
            if(!server_.waitForNewConnection(10000)) {
                cerr << "[giw] No clients connected to ImageWatch server" << endl;
            }
            client_ = server_.nextPendingConnection();
        }
    }
};


AppHandler giw_initialize(int(*plot_callback)(const char*))
{
    GiwBridge *app = new GiwBridge(plot_callback);
    return static_cast<AppHandler>(app);
}


void giw_cleanup(AppHandler handler)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_terminate received null application handler");
        return;
    }

    delete app;
}


void giw_exec(AppHandler handler)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_exec received null application handler");
        return;
    }

    app->start();
}


int giw_is_window_ready(AppHandler handler)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_exec received null application handler");
        return 0;
    }

    return app->is_window_ready();
}


PyObject* giw_get_observed_buffers(AppHandler handler)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_Exception,
                           "giw_get_observed_buffers received null "
                           "application handler");
        return nullptr;
    }

    auto observed_symbols         = app->get_observed_symbols();
    PyObject* py_observed_symbols = PyList_New(observed_symbols.size());

    int observed_symbols_sentinel = static_cast<int>(observed_symbols.size());
    for (int i = 0; i < observed_symbols_sentinel; ++i) {
        string symbol_name       = observed_symbols[i];
        PyObject* py_symbol_name = PyBytes_FromString(symbol_name.c_str());

        if (py_symbol_name == nullptr) {
            Py_DECREF(py_observed_symbols);
            return nullptr;
        }

        PyList_SetItem(py_observed_symbols, i, py_symbol_name);
    }

    return py_observed_symbols;
}


void giw_set_available_symbols(AppHandler handler,
                               PyObject* available_vars_py)
{
    assert(PyList_Check(available_vars_py));

    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_set_available_symbols received null "
                           "application handler");
        return;
    }

    deque<string> available_vars_stl;
    for (Py_ssize_t pos = 0; pos < PyList_Size(available_vars_py); ++pos) {
        string var_name_str;
        PyObject* listItem = PyList_GetItem(available_vars_py, pos);
        copy_py_string(var_name_str, listItem);
        available_vars_stl.push_back(var_name_str);
    }

    app->set_available_symbols(available_vars_stl);
}


void giw_run_event_loop(AppHandler handler)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if(app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_run_event_loop received null application "
                           "handler");
        return;
    }

    app->run_event_loop();
}


void giw_plot_buffer(AppHandler handler, PyObject* buffer_metadata)
{
    GiwBridge* app = static_cast<GiwBridge*>(handler);

    if (app == nullptr) {
        RAISE_PY_EXCEPTION(PyExc_RuntimeError,
                           "giw_plot_buffer received null application handler");
        return;
    }

    if (!PyDict_Check(buffer_metadata)) {
        RAISE_PY_EXCEPTION(PyExc_TypeError,
                           "Invalid object given to plot_buffer (was expecting"
                           " a dict).");
        return;
    }

    /*
     * Get required fields
     */
    PyObject* py_variable_name =
        PyDict_GetItemString(buffer_metadata, "variable_name");
    PyObject* py_display_name =
        PyDict_GetItemString(buffer_metadata, "display_name");
    PyObject* py_pointer  = PyDict_GetItemString(buffer_metadata, "pointer");
    PyObject* py_width    = PyDict_GetItemString(buffer_metadata, "width");
    PyObject* py_height   = PyDict_GetItemString(buffer_metadata, "height");
    PyObject* py_channels = PyDict_GetItemString(buffer_metadata, "channels");
    PyObject* py_type     = PyDict_GetItemString(buffer_metadata, "type");
    PyObject* py_row_stride =
        PyDict_GetItemString(buffer_metadata, "row_stride");
    PyObject* py_pixel_layout =
        PyDict_GetItemString(buffer_metadata, "pixel_layout");

    /*
     * Get optional fields
     */
    PyObject* py_transpose_buffer =
        PyDict_GetItemString(buffer_metadata, "transpose_buffer");
    bool transpose_buffer = false;
    if (py_transpose_buffer != nullptr) {
        CHECK_FIELD_TYPE(transpose_buffer, PyBool_Check, "transpose_buffer");
        transpose_buffer = PyObject_IsTrue(py_transpose_buffer);
    }

    /*
     * Check if expected fields were provided
     */
    CHECK_FIELD_PROVIDED(variable_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(display_name, "plot_buffer");
    CHECK_FIELD_PROVIDED(pointer, "plot_buffer");
    CHECK_FIELD_PROVIDED(width, "plot_buffer");
    CHECK_FIELD_PROVIDED(height, "plot_buffer");
    CHECK_FIELD_PROVIDED(channels, "plot_buffer");
    CHECK_FIELD_PROVIDED(type, "plot_buffer");
    CHECK_FIELD_PROVIDED(row_stride, "plot_buffer");
    CHECK_FIELD_PROVIDED(pixel_layout, "plot_buffer");

    /*
     * Check if expected fields have the correct types
     */
    CHECK_FIELD_TYPE(variable_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(display_name, check_py_string_type, "plot_buffer");
    CHECK_FIELD_TYPE(pointer, PyMemoryView_Check, "plot_buffer");
    CHECK_FIELD_TYPE(width, PyLong_Check, "plot_buffer");
    CHECK_FIELD_TYPE(height, PyLong_Check, "plot_buffer");
    CHECK_FIELD_TYPE(channels, PyLong_Check, "plot_buffer");
    CHECK_FIELD_TYPE(type, PyLong_Check, "plot_buffer");
    CHECK_FIELD_TYPE(row_stride, PyLong_Check, "plot_buffer");
    CHECK_FIELD_TYPE(pixel_layout, check_py_string_type, "plot_buffer");

    /*
     * Enqueue provided fields so the request can be processed in the main
     * thread
     */
    BufferRequestMessage request(py_pointer,
                                 py_variable_name,
                                 py_display_name,
                                 get_py_int(py_width),
                                 get_py_int(py_height),
                                 get_py_int(py_channels),
                                 get_py_int(py_type),
                                 get_py_int(py_row_stride),
                                 py_pixel_layout,
                                 transpose_buffer);

    //app->plot_buffer(request);
}