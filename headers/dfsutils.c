#include "dfsutils.h"

int get_dfs_socket(int port_number)
{

  int sockfd;
  struct sockaddr_in sin;
  int yes = 1;

  memset(&sin, 0, sizeof(sin));

  sin.sin_family = AF_INET;
  sin.sin_port = htons(port_number);
  sin.sin_addr.s_addr = INADDR_ANY;

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("Unable to start socket:");
    exit(1);
  }

  // Avoiding the "Address Already in use" error message
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
    perror("Unable to set so_reuseaddr:");
    exit(1);
  }

  if (bind(sockfd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
    perror("Unable to bind the socket:");
    exit(1);
  }

  if (listen(sockfd, MAXCONNECTION) < 0) {
    perror("Unable to call listen on socket:");
    exit(1);
  }

  return sockfd;
}
bool dfs_command_decode_and_auth(char* buffer, const char* format, dfs_recv_command_struct* recv_cmd, dfs_conf_struct* conf)
{
  // All folder ends with '/'
  // saving values needed to evaluate command
  int flag;
  user_struct* user;
  user = &recv_cmd->user;
  sscanf(buffer, format,
      &flag,
      user->username,
      user->password,
      recv_cmd->folder,
      recv_cmd->file_name);

  //  Handle the case of receiving "NULL" on client side, no info on servers how to process NULL
  if (compare_str(recv_cmd->folder, "NULL")) {
    memset(recv_cmd->folder, 0, sizeof(recv_cmd->folder));
  }

  if (compare_str(recv_cmd->file_name, "NULL")) {
    memset(recv_cmd->file_name, 0, sizeof(recv_cmd->file_name));
  }
  //DEBUGSS("Username", user->username);
  //DEBUGSS("Password", user->password);
  //DEBUGSS("Folder", recv_cmd->folder);
  //DEBUGSS("Filename", recv_cmd->file_name);

  return auth_dfs_user(user, conf);
}

void dfs_command_accept(int socket, dfs_conf_struct* conf)
{
  char buffer[MAX_SEG_SIZE], temp_buffer[MAX_SEG_SIZE];
  bool auth_flag;
  int flag, command_size;
  user_struct* user;
  dfs_recv_command_struct dfs_recv_command;

  memset(buffer, 0, sizeof(buffer));
  memset(temp_buffer, 0, sizeof(temp_buffer));
  memset(&dfs_recv_command, 0, sizeof(dfs_recv_command));

  if ((dfs_recv_command.user.username = (char*)malloc(MAXCHARBUFF * sizeof(char))) == NULL) {
    DEBUGSS("Failed to malloc", strerror(errno));
  }
  if ((dfs_recv_command.user.password = (char*)malloc(MAXCHARBUFF * sizeof(char))) == NULL) {
    DEBUGSS("Failed to malloc", strerror(errno));
  }

  user = &dfs_recv_command.user;

  // receiving the command
  // Receiving size of command followd by command
  recv_int_value_socket(socket, &command_size);
  recv_from_socket(socket, buffer, command_size);

  // Received buffer ends with '\n' to assist easy split
  sscanf(buffer, GENERIC_TEMPATE, &dfs_recv_command.flag, temp_buffer);
  flag = dfs_recv_command.flag;

  DEBUGS("Decoding and authentication command");
  // Handle case when Authentication Fails
  if (flag == LIST_FLAG) {
    DEBUGS("Command Received is LIST");
    auth_flag = dfs_command_decode_and_auth(buffer, LIST_TEMPLATE, &dfs_recv_command, conf);

  } else if (flag == GET_FLAG) {

    DEBUGS("Command Received is GET");
    auth_flag = dfs_command_decode_and_auth(buffer, GET_TEMPLATE, &dfs_recv_command, conf);

  } else if (flag == PUT_FLAG) {

    DEBUGS("Command Received is PUT");
    auth_flag = dfs_command_decode_and_auth(buffer, PUT_TEMPLATE, &dfs_recv_command, conf);

  } else if (flag == MKDIR_FLAG) {

    DEBUGS("Command Received is MKDIR");
    auth_flag = dfs_command_decode_and_auth(buffer, MKDIR_TEMPLATE, &dfs_recv_command, conf);
  }

  if (!auth_flag) {
    send_int_value_socket(socket, -1);
    send_error(socket, AUTH_FAILED);
  } else
    dfs_command_exec(socket, &dfs_recv_command, conf, flag);

  free(dfs_recv_command.user.username);
  free(dfs_recv_command.user.password);

  //DEBUGS("Command Execution Done");
}

void send_error_helper(int socket, const char* message)
{

  int payload_size;
  u_char* payload;

  payload_size = strlen(message);
  if ((payload = (u_char*)malloc(payload_size * sizeof(u_char))) == NULL) {
    DEBUGSS("Failed to malloc", strerror(errno));
  }
  memcpy(payload, message, payload_size);
  send_int_value_socket(socket, payload_size);
  send_to_socket(socket, payload, payload_size);

  free(payload);
}
void send_error(int socket, int flag)
{

  if (flag == FOLDER_NOT_FOUND) {
    send_error_helper(socket, FOLDER_NOT_FOUND_ERROR);
  } else if (flag == FOLDER_EXISTS) {
    send_error_helper(socket, FOLDER_EXISTS_ERROR);
  } else if (flag == FILE_NOT_FOUND) {
    send_error_helper(socket, FILE_NOT_FOUND_ERROR);
  } else if (flag == AUTH_FAILED) {
    send_error_helper(socket, AUTH_FAILED_ERROR);
  } else {
    DEBUGS("Unknown Error Flag");
  }
}

bool dfs_command_exec(int socket, dfs_recv_command_struct* recv_cmd, dfs_conf_struct* conf, int flag)
{
  // Executes the received command
  char folder_path[2 * MAXCHARBUFF], signal;
  u_char payload_buffer[MAX_SEG_SIZE], *u_char_buffer;
  server_chunks_info_struct server_chunks_info;
  split_struct splits[2];
  int len, i, size_of_payload, split_id;
  bool folder_path_flag, file_flag;

  memset(folder_path, 0, 2 * MAXCHARBUFF * sizeof(char));
  memset(&splits, 0, 2 * sizeof(split_struct));
  memset(&server_chunks_info, 0, sizeof(server_chunks_info_struct));

  // Creating username directory in case it doesn't exist already
  len = sprintf(folder_path, "%s/%s", conf->server_name, recv_cmd->user.username);

  if (!check_directory_exists(folder_path)) {
    DEBUGSS("Creating user directory:", folder_path);
    create_dfs_directory(folder_path);
  }

  // Since recv_cmd->folder ends with a '/' no need to add it in format
  len = sprintf(folder_path, "%s/%s/%s", conf->server_name, recv_cmd->user.username, recv_cmd->folder);

  // Handling case when recv_cmd->folder is '/'
  if (folder_path[len - 1] == ROOT_FOLDER_CHR && folder_path[len - 2] == ROOT_FOLDER_CHR)
    folder_path[--len] = NULL_CHAR;

  DEBUGSS("Folder Path from Request", folder_path);

  // Check if folder path exists
  folder_path_flag = check_directory_exists(folder_path);
  if (flag == LIST_FLAG) {

    if (!folder_path_flag) {
      DEBUGS("Folder path doesn't exist and sending back error message");
      send_int_value_socket(socket, -1);
      send_error(socket, FOLDER_NOT_FOUND);
      return false;
    }

    send_int_value_socket(socket, 1);
    DEBUGS("Reading all the files in the folder path from request");
    get_files_in_folder(folder_path, &server_chunks_info, NULL);

    DEBUGS("Sending files info to the client");
    // Estimate the size of payload to send
    size_of_payload = INT_SIZE + server_chunks_info.chunks * CHUNK_INFO_STRUCT_SIZE;

    // Sending value of payload to expect
    send_int_value_socket(socket, size_of_payload);

    // encoding server_chunks_info into u_char
    if ((u_char_buffer = (u_char*)malloc(sizeof(u_char) * size_of_payload)) == NULL) {
      DEBUGSS("Failed to malloc", strerror(errno));
    }
    memset(u_char_buffer, 0, size_of_payload * sizeof(u_char));
    encode_server_chunks_info_struct_to_buffer(u_char_buffer, &server_chunks_info);

    // Sending the encoded buffer over socket
    send_to_socket(socket, u_char_buffer, size_of_payload);
    free(u_char_buffer);

    size_of_payload = get_folders_in_folder(folder_path, &u_char_buffer);

    send_int_value_socket(socket, size_of_payload);
    send_to_socket(socket, u_char_buffer, size_of_payload);

    free(u_char_buffer);
    free(server_chunks_info.chunk_info);
  } else if (flag == GET_FLAG) {

    if (!folder_path_flag) {
      DEBUGS("Folder path doesn't exist and sending back error message");
      send_int_value_socket(socket, -1);
      send_error(socket, FOLDER_NOT_FOUND);
      return false;
    }

    DEBUGS("Reading given file from folder path from request");

    // Handle case when file doesn't exists
    file_flag = get_files_in_folder(folder_path, &server_chunks_info, recv_cmd->file_name);
    /*print_server_chunks_info_struct(&server_chunks_info);*/
    if (!file_flag) {
      DEBUGS("File doesn't exist and sending back error message");
      send_int_value_socket(socket, -1);
      send_error(socket, FILE_NOT_FOUND);
      return false;
    }
    /*print_server_chunks_info_struct(&server_chunks_info);*/
    // Estimate the size of payload to send
    size_of_payload = INT_SIZE + server_chunks_info.chunks * CHUNK_INFO_STRUCT_SIZE;

    send_int_value_socket(socket, 1);
    DEBUGS("Sending the file's info to the client");
    // Sending the size of payload to expect
    send_int_value_socket(socket, size_of_payload);

    // Encoding the server_chunks_info into buffer
    if ((u_char_buffer = (u_char*)malloc(sizeof(u_char) * size_of_payload)) == NULL) {
      DEBUGSS("Failed to malloc", strerror(errno));
    }

    memset(u_char_buffer, 0, size_of_payload * sizeof(u_char));
    encode_server_chunks_info_struct_to_buffer(u_char_buffer, &server_chunks_info);

    // Sending the encoded buffer
    send_to_socket(socket, u_char_buffer, size_of_payload);
    free(u_char_buffer);

    DEBUGS("Waiting for signal from client");
    recv_signal(socket, &signal);
    free(server_chunks_info.chunk_info);
    if (signal == PROCEED_SIG) {
      // User wants to fetch the files
      // Possible that user requests for more than one chunk per server

      DEBUGS("Proceeding with sending file split as requested by client");
      while (true) {

        // Recv the split id to send
        recv_int_value_socket(socket, &split_id);
        //DEBUGSN("Split #", split_id);

        sprintf(folder_path + len, ".%s.%d", recv_cmd->file_name, split_id);
        //DEBUGSS("Reading split from path", folder_path);

        splits->id = split_id;
        read_into_split_from_file(folder_path, &splits[0]);
        /*print_split_struct(splits);*/
        write_split_to_socket_as_stream(socket, splits);
        recv_signal(socket, &signal);

        free_split_struct(&splits[0]);
        if (signal == RESET_SIG)
          break;
      }

    } else {

      DEBUGS("Client sent RESET SIG not proceeding");
      // Let the server exit stop the connections
    }

  } else if (flag == PUT_FLAG) {

    if (!folder_path_flag) {
      DEBUGS("Folder path doesn't exist and sending back error message");
      send_int_value_socket(socket, -1);
      send_error(socket, FOLDER_NOT_FOUND);
      return false;
    }

    send_int_value_socket(socket, 1);
    DEBUGS("Reading splits from the socket and writing to the file path");
    for (i = 0; i < 2; i++) {
      // Checked if the path exits before executing the command
      memset(payload_buffer, 0, MAX_SEG_SIZE * sizeof(u_char));
      write_split_from_socket_as_stream(socket, &splits[i]);
      write_split_struct_to_file(&splits[i], folder_path, recv_cmd->file_name);
      free_split_struct(&splits[i]);
    }
  } else if (flag == MKDIR_FLAG) {

    // Check if parent directory exits before proceeding

    if (folder_path_flag) {
      DEBUGS("Folder path already exists");
      send_int_value_socket(socket, -1);
      send_error(socket, FOLDER_EXISTS);
      return false;
    }
    send_int_value_socket(socket, 1);
    DEBUGS("Creating director");
    create_dfs_directory(folder_path);
  }

  return true;
}

bool auth_dfs_user(user_struct* user, dfs_conf_struct* conf)
{
  int i;
  for (i = 0; i < conf->user_count; i++) {
    if (compare_user_struct(user, conf->users[i])) {
      //DEBUGSS("auth_dfs_user: Authenticated", user->username);
      return true;
    }
  }
  //DEBUGSS("Couldn't Authenticate", user->username);
  return false;
}

void read_dfs_conf(char* file_path, dfs_conf_struct* conf)
{
  FILE* fp;
  char line[MAXFILEBUFF];
  int line_len;
  if ((fp = fopen(file_path, "r")) <= 0) {
    perror("DFC => Error in opening config file: ");
  }

  while (fgets(line, sizeof(line), fp)) {
    // Assumption that entire line can fix in this buffer
    line_len = strlen(line);
    line[line_len - 1] = (line[line_len - 1] == NEW_LINE_CHAR) ? NULL_CHAR : line[line_len - 1];
    insert_dfs_user_conf(line, conf);
  }
}

void insert_dfs_user_conf(char* line, dfs_conf_struct* conf)
{
  char* ptr;
  int len, i;
  ptr = get_sub_string_after(line, SPACE_STR);
  len = ptr - line - 1;

  i = conf->user_count++;
  assert(check_user_struct(&conf->users[i]) == false);
  conf->users[i]->username = strndup(line, len);
  conf->users[i]->password = strdup(ptr);
}

void create_dfs_directory(char* path)
{
  struct stat st;

  if (stat(path, &st) == -1)
    mkdir(path, 0755);
}

// Creates directory for all the servers with sub directory for each of the user
void dfs_directory_creator(char* server_name, dfs_conf_struct* conf)
{
  int i;
  char file_path[MAXFILEBUFF];
  user_struct* user;
  create_dfs_directory(server_name);
  for (i = 0; i < conf->user_count; i++) {
    user = conf->users[i];
    memset(file_path, 0, sizeof(file_path));
    sprintf(file_path, "%s/%s", server_name, user->username);
    create_dfs_directory(file_path);
  }
}

void print_dfs_conf_struct(dfs_conf_struct* conf)
{
  int i;
  user_struct* ptr;
  for (i = 0; i < conf->user_count; i++) {
    ptr = conf->users[i];
    fprintf(stderr, "Username: %s & Password: %s\n", ptr->username, ptr->password);
  }
}

void free_dfs_conf_struct(dfs_conf_struct* conf)
{
  int i;
  i = conf->user_count;
  while (i--)
    free_user_struct(conf->users[i]);
}
