/*
 * Copyright (C) 2021 Liquidaty and zsv contributors. All rights reserved.
 *
 * This file is part of zsv/lib, distributed under the MIT license as defined at
 * https://opensource.org/licenses/MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>

#define _GNU_SOURCE 1
#include <string.h>
#include <ctype.h>

#define ZSV_COMMAND echo
#include "zsv_command.h"

#include <zsv/utils/compiler.h>
#include <zsv/utils/writer.h>
#include <zsv/utils/string.h>
#include <zsv/utils/mem.h>

enum zsv_echo_overwrite_input_type {
  zsv_echo_overwrite_input_type_sqlite3 = 0
};

struct zsv_echo_data {
  FILE *in;
  const char *input_path;
  zsv_csv_writer csv_writer;
  zsv_parser parser;
  size_t row_ix;

  struct {
    size_t row_ix;
    size_t col_ix;
    const unsigned char *str;
    size_t len;
    char eof;
  } overwrite;

  enum zsv_echo_overwrite_input_type overwrite_input_type;
  struct {
    struct {
      char *filename;
      sqlite3 *db;
      sqlite3_stmt *stmt; // select row, column, overwrite
      const char *sql;
    } sqlite3;
  } o;
};

/**
 * check if sqlite3 statement is valid
 * return error
 */
static int zsv_echo_sqlite3_check_stmt(sqlite3_stmt *stmt) {
  if(sqlite3_column_count(stmt) < 3)
    return 1;
  // to do: check that columns are row, column, value
  return 0;
}

/**
 * to do: verify original value
 */
void zsv_echo_get_next_overwrite(struct zsv_echo_data *data) {
  if(!data->overwrite.eof) {
    sqlite3_stmt *stmt = data->o.sqlite3.stmt;
    if(stmt) {
      if(sqlite3_step(stmt) == SQLITE_ROW) {
        // row, column, value
        data->overwrite.row_ix = sqlite3_column_int64(stmt, 0);
        data->overwrite.col_ix = sqlite3_column_int64(stmt, 1);
        data->overwrite.str = sqlite3_column_text(stmt, 2);
        data->overwrite.len = sqlite3_column_bytes(stmt, 2);
      } else
        data->overwrite.eof = 1;
    }
  }
}

static void zsv_echo_row(void *hook) {
  struct zsv_echo_data *data = hook;
  if(VERY_UNLIKELY(data->row_ix == 0)) { // header
    for(size_t i = 0, j = zsv_cell_count(data->parser); i < j; i++) {
      struct zsv_cell cell = zsv_get_cell(data->parser, i);
      zsv_writer_cell(data->csv_writer, i == 0, cell.str, cell.len, cell.quoted);
    }
  } else {
    for(size_t i = 0, j = zsv_cell_count(data->parser); i < j; i++) {
      if(data->overwrite.row_ix == data->row_ix && data->overwrite.col_ix == i) {
        zsv_writer_cell(data->csv_writer, i == 0, data->overwrite.str, data->overwrite.len, 1);
        zsv_echo_get_next_overwrite(data);
      } else {
        struct zsv_cell cell = zsv_get_cell(data->parser, i);
        zsv_writer_cell(data->csv_writer, i == 0, cell.str, cell.len, cell.quoted);
      }
    }
    while(!data->overwrite.eof && data->overwrite.row_ix <= data->row_ix)
      zsv_echo_get_next_overwrite(data);
  }
  data->row_ix++;
}

const char *zsv_echo_usage_msg[] = {
  APPNAME ": write tabular input to stdout with optional cell overwrites",
  "",
  "Usage: " APPNAME " [filename] [--overwrite <overwrite-source>]",
  "",
  "Options:",
  "  -b                  : output with BOM",
  "  --overwrite <source>: overwrite cells using given source. Source may be:",
  "                        - sqlite3://<filename>[?sql=<query>]",
  "                          ex: sqlite3://overwrites.db?sql=select row, column, value from overwrites order by row, column",
  NULL
};

static int zsv_echo_usage() {
  for(int i = 0; zsv_echo_usage_msg[i]; i++)
    fprintf(stdout, "%s\n", zsv_echo_usage_msg[i]);
  return 1;
}

static void zsv_echo_cleanup(struct zsv_echo_data *data) {
  zsv_writer_delete(data->csv_writer);
  free(data->o.sqlite3.filename);
  if(data->o.sqlite3.stmt)
    sqlite3_finalize(data->o.sqlite3.stmt);
  if(data->in && data->in != stdin)
    fclose(data->in);
  if(data->o.sqlite3.db)
    sqlite3_close(data->o.sqlite3.db);
}

static int zsv_echo_parse_overwrite_source(struct zsv_echo_data *data, const char *source, size_t len) {
#define zsv_echo_sqlite3_prefix "sqlite3://"
  size_t pfx_len;
  if(len > (pfx_len = strlen(zsv_echo_sqlite3_prefix)) && !memcmp(source, zsv_echo_sqlite3_prefix, pfx_len)) {
    data->o.sqlite3.filename = zsv_memdup(source + pfx_len, len - pfx_len);
    char *q = memchr(data->o.sqlite3.filename, '?', len - pfx_len);
    if(q) {
      *q = '\0';
      q++;
#define zsv_echo_sql_prefix "sql="
      const char *sql = strstr(q, zsv_echo_sql_prefix);
      if(sql)
        data->o.sqlite3.sql = sql + strlen(zsv_echo_sql_prefix);
    }
    // open the sql connection
    if(!(data->o.sqlite3.filename && *data->o.sqlite3.filename)) {
      fprintf(stderr, "Invalid query string");
      return 1;
    }

    int rc = sqlite3_open_v2(data->o.sqlite3.filename, &data->o.sqlite3.db, SQLITE_OPEN_READONLY, NULL);
    if(rc != SQLITE_OK || !data->o.sqlite3.db) {
      fprintf(stderr, "%s: %s\n", sqlite3_errstr(rc), data->o.sqlite3.filename);
      return 1;
    }

    if(!data->o.sqlite3.sql) {
      // to do: detect it from the db
      fprintf(stderr, "Missing sql select statement for sqlite3 echo data e.g.:\n"
              "  select row, column, value from overwrites order by row, column\n");
      return 1;
    }

    rc = sqlite3_prepare_v2(data->o.sqlite3.db, data->o.sqlite3.sql, -1, &data->o.sqlite3.stmt, NULL);
    if(rc != SQLITE_OK || !data->o.sqlite3.stmt) {
      fprintf(stderr, "%s\n", sqlite3_errmsg(data->o.sqlite3.db));
      return 1;
    }

    // successful sqlite3 connection
    data->overwrite.eof = 0;
    return 0;
  }

  fprintf(stderr, "Invalid overwrite source: %s\n", source);
  return 1;
}

int ZSV_MAIN_FUNC(ZSV_COMMAND)(int argc, const char *argv[], struct zsv_opts *opts, const char *opts_used) {
  if(argc < 1 || (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
    zsv_echo_usage();
    return 0;
  }

  struct zsv_csv_writer_options writer_opts = zsv_writer_get_default_opts();
  struct zsv_echo_data data = { 0 };
  data.overwrite_input_type = zsv_echo_overwrite_input_type_sqlite3;

  int err = 0;

  data.overwrite.eof = 1;
  for(int arg_i = 1; !err && arg_i < argc; arg_i++) {
    const char *arg = argv[arg_i];
    if(!strcmp(arg, "-b"))
      writer_opts.with_bom = 1;
    else if(!strcmp(arg, "--overwrite")) {
      if(arg_i+1 >= argc) {
        fprintf(stderr, "Option %s requires a value\n", arg);
        err = 1;
      } else {
        const char *src = argv[++arg_i];
        err = zsv_echo_parse_overwrite_source(&data, src, strlen(src));
      }
    } else if(!data.in) {
#ifndef NO_STDIN
      if(!strcmp(arg, "-"))
        data.in = stdin;
#endif
      if(!data.in) {
        if(!(data.in = fopen(arg, "rb"))) {
          err = 1;
          perror(arg);
        } else
          data.input_path = arg;
      }
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", arg);
      err = 1;
    }
  }

  if(!err && !data.in) {
#ifndef NO_STDIN
    data.in = stdin;
#else
    fprintf(stderr, "No input\n");
    err = 1;
#endif
  }

  if(err) {
    zsv_echo_cleanup(&data);
    return 1;
  }

  opts->row_handler = zsv_echo_row;
  opts->stream = data.in;
  opts->ctx = &data;
  data.csv_writer = zsv_writer_new(&writer_opts);
  if(zsv_new_with_properties(opts, data.input_path, opts_used, &data.parser) != zsv_status_ok
     || !data.csv_writer) {
    zsv_echo_cleanup(&data);
    return 1;
  }

  // create a local csv writer buff for faster performance
  unsigned char writer_buff[64];
  zsv_writer_set_temp_buff(data.csv_writer, writer_buff, sizeof(writer_buff));

  // process the input data.
  zsv_handle_ctrl_c_signal();
  enum zsv_status status;
  while(!zsv_signal_interrupted && (status = zsv_parse_more(data.parser)) == zsv_status_ok)
    ;

  zsv_finish(data.parser);
  zsv_delete(data.parser);
  zsv_echo_cleanup(&data);
  return 0;
}
