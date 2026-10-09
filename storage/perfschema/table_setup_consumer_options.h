/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef TABLE_SETUP_CONSUMER_OPTIONS_H
#define TABLE_SETUP_CONSUMER_OPTIONS_H

/**
  @file storage/perfschema/table_setup_consumer_options.h
  Percona: table SETUP_CONSUMER_OPTIONS (declarations).
*/

#include <sys/types.h>

#include "lex_string.h"
#include "my_base.h"
#include "storage/perfschema/pfs_engine_table.h"
#include "storage/perfschema/table_helper.h"

class Field;
class Plugin_table;
struct TABLE;
struct THR_LOCK;

/**
  @addtogroup performance_schema_tables
  @{
*/

/** A row of PERFORMANCE_SCHEMA.SETUP_CONSUMER_OPTIONS. */
struct row_setup_consumer_options {
  /** Column CONSUMER. */
  LEX_CSTRING m_consumer;
  /** Column NAME. */
  LEX_CSTRING m_name;
  /** Allowed values of column VALUE, null terminated. */
  const LEX_CSTRING *m_values;
  /** Index of the current value in m_values. */
  ulong *m_value_ptr;
  /** Called after the value changed. */
  void (*m_refresh)();
};

class PFS_index_setup_consumer_options : public PFS_engine_index {
 public:
  PFS_index_setup_consumer_options()
      : PFS_engine_index(&m_key_1, &m_key_2),
        m_key_1("CONSUMER"),
        m_key_2("NAME") {}

  ~PFS_index_setup_consumer_options() override = default;

  virtual bool match(const row_setup_consumer_options *row);

 private:
  PFS_key_name m_key_1;
  PFS_key_name m_key_2;
};

/** Table PERFORMANCE_SCHEMA.SETUP_CONSUMER_OPTIONS. */
class table_setup_consumer_options : public PFS_engine_table {
 public:
  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table *create(PFS_engine_table_share *);
  static ha_rows get_row_count();

  void reset_position() override;

  int rnd_next() override;
  int rnd_pos(const void *pos) override;

  int index_init(uint idx, bool sorted) override;
  int index_next() override;

 protected:
  int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all) override;

  int update_row_values(TABLE *table, const unsigned char *old_buf,
                        unsigned char *new_buf, Field **fields) override;
  table_setup_consumer_options();

 public:
  ~table_setup_consumer_options() override = default;

 private:
  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Table definition. */
  static Plugin_table m_table_def;

  /** Current row. */
  const row_setup_consumer_options *m_row;
  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;

  PFS_index_setup_consumer_options *m_opened_index;
};

/** @} */
#endif
