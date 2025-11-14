CC = gcc
CFLAGS = -Wall -Wextra -I/usr/include
LIBS = -lm -lraylib -lpthread -ldl -lrt -lX11

SERVER_TARGET = server_gui
SERVER_SOURCE = server_gui.c server.c message.c file_transfer.c file_transfer_protocol.c sliding_window.c

# Headless server CLI
SERVER_CLI_TARGET = server
SERVER_CLI_SOURCE = server_cli.c server.c message.c file_transfer.c file_transfer_protocol.c sliding_window.c


CLIENT_TARGET = client_gui
CLIENT_SOURCE = client_gui.c warning_dialog.c client_network.c message.c file_transfer.c file_transfer_protocol.c sliding_window.c

all: $(SERVER_TARGET) $(CLIENT_TARGET)

$(SERVER_TARGET): $(SERVER_SOURCE)
	$(CC) $(CFLAGS) $(SERVER_SOURCE) $(LIBS) -o $(SERVER_TARGET) $(LIBS)

$(SERVER_CLI_TARGET): $(SERVER_CLI_SOURCE)
	$(CC) $(CFLAGS) $(SERVER_CLI_SOURCE) $(LIBS) -o $(SERVER_CLI_TARGET) $(LIBS)


$(CLIENT_TARGET): $(CLIENT_SOURCE)
	$(CC) $(CFLAGS) $(CLIENT_SOURCE) $(LIBS) -o $(CLIENT_TARGET) $(LIBS)

client: $(CLIENT_TARGET)

# Keep a convenience alias for building the GUI server
server_gui_build: $(SERVER_TARGET)

clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET) $(SERVER_CLI_TARGET)
