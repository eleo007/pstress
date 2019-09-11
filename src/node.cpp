#include "node.hpp"
#include "common.hpp"
#include "random_test.hpp"
#include <cerrno>
#include <cstring>
#include <iostream>

Node::Node() {
  workers.clear();
  performed_queries_total = 0;
  failed_queries_total = 0;
}

Node::~Node() {
  writeFinalReport();
  if (general_log) {
    general_log.close();
  }
  /* if mode is query generator*/
  if (options->at(Option::DYNAMIC_PQUERY)) {
    save_objects_to_file();
    clean_up_at_end();
  } else {
    if (querylist) {
      delete querylist;
    }
  }
}
bool Node::createGeneralLog() {
  std::string logName;
  logName = myParams.logdir + "/" + myParams.myName + "_general" + ".log";
  general_log.open(logName, std::ios::out | std::ios::trunc);
  general_log << "- PQuery v" << PQVERSION << "-" << PQREVISION
              << " compiled with " << FORK << "-" << mysql_get_client_info()
              << std::endl;

  if (!general_log.is_open()) {
    std::cout << "Unable to open log file " << logName << ": "
              << std::strerror(errno) << std::endl;
    return false;
  }
  return true;
}

void Node::writeFinalReport() {
  if (general_log.is_open()) {
    std::ostringstream exitmsg;
    exitmsg.precision(2);
    exitmsg << std::fixed;

    if (options->at(Option::DYNAMIC_PQUERY)->getBool() == false) {
      exitmsg << "* NODE SUMMARY: " << failed_queries_total << "/"
              << performed_queries_total << " queries failed, ("
              << (performed_queries_total - failed_queries_total) * 100.0 /
                     performed_queries_total
              << "% were successful)";
    } else {
      unsigned long int success_queries = 0;
      unsigned long int total_queries = 0;
      for (auto op : *options) {
        if (op == nullptr)
          continue;
        if (op->total_queries > 0) {
          total_queries += op->total_queries;
          success_queries += op->success_queries;
          general_log << op->help << ", total=>" << op->total_queries
                      << ", success=> " << op->success_queries << std::endl;
        }
      }

      unsigned long int percentage =
          total_queries == 0 ? 0 : success_queries * 100 / total_queries;

      exitmsg << "* SUMMAR: " << total_queries - success_queries << "/"
              << total_queries << "queries failed, (" << percentage
              << "% were successful)";
    }
      general_log << exitmsg.str() << std::endl;
      exitmsg.str(std::string());
  }
}

int Node::startWork() {

  if (!createGeneralLog()) {
    std::cerr << "Exiting..." << std::endl;
    return 2;
  }

  std::cout << "- Connecting to " << myParams.myName << " [" << myParams.address
            << "]..." << std::endl;
  general_log << "- Connecting to " << myParams.myName << " ["
              << myParams.address << "]..." << std::endl;
  tryConnect();

  if (options->at(Option::DYNAMIC_PQUERY)->getBool() == false) {
    std::ifstream sqlfile_in;
    sqlfile_in.open(myParams.infile);

    if (!sqlfile_in.is_open()) {
      std::cerr << "Unable to open SQL file " << myParams.infile << ": "
                << strerror(errno) << std::endl;
      general_log << "Unable to open SQL file " << myParams.infile << ": "
                  << strerror(errno) << std::endl;
      return EXIT_FAILURE;
    }
    querylist = new std::vector<std::string>;
    std::string line;

    while (getline(sqlfile_in, line)) {
      if (!line.empty()) {
        querylist->push_back(line);
      }
    }

    sqlfile_in.close();
    general_log << "- Read " << querylist->size() << " lines from "
                << myParams.infile << std::endl;

    /* log replaying */
    if (options->at(Option::NO_SHUFFLE)->getBool()) {
      myParams.threads = 1;
      myParams.queries_per_thread = querylist->size();
    }
  }
  /* END log replaying */
  workers.resize(myParams.threads);

  for (int i = 0; i < myParams.threads; i++) {
    workers[i] = std::thread(&Node::workerThread, this, i);
  }

  for (int i = 0; i < myParams.threads; i++) {
    workers[i].join();
  }
  return EXIT_SUCCESS;
}

void Node::tryConnect() {
  MYSQL *conn;
  conn = mysql_init(NULL);
  if (conn == NULL) {
    std::cerr << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
              << std::endl;
    std::cerr << "* PQUERY: Unable to continue [1], exiting" << std::endl;
    general_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
                << std::endl;
    general_log << "* PQUERY: Unable to continue [1], exiting" << std::endl;
    mysql_close(conn);
    mysql_library_end();
    exit(EXIT_FAILURE);
  }
  if (mysql_real_connect(conn, myParams.address.c_str(),
                         myParams.username.c_str(), myParams.password.c_str(),
                         options->at(Option::DATABASE)->getString().c_str(),
                         myParams.port, myParams.socket.c_str(), 0) == NULL) {
    std::cerr << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
              << std::endl;
    std::cerr << "* PQUERY: Unable to continue [2], exiting" << std::endl;
    general_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
                << std::endl;
    general_log << "* PQUERY: Unable to continue [2], exiting" << std::endl;
    mysql_close(conn);
    mysql_library_end();
    exit(EXIT_FAILURE);
  }
  general_log << "- Connected to " << mysql_get_host_info(conn) << "..."
              << std::endl;
  // getting the real server version
  MYSQL_RES *result = NULL;
  std::string server_version;

  if (!mysql_query(conn, "select @@version_comment limit 1") &&
      (result = mysql_use_result(conn))) {
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row && row[0]) {
      server_version = mysql_get_server_info(conn);
      server_version.append(" ");
      server_version.append(row[0]);
    }
  } else {
    server_version = mysql_get_server_info(conn);
  }
  general_log << "- Connected server version: " << server_version << std::endl;
  if (result != NULL) {
    mysql_free_result(result);
  }
  mysql_close(conn);
  if (options->at(Option::TEST_CONNECTION)->getBool()) {
    exit(EXIT_SUCCESS);
  }
}

