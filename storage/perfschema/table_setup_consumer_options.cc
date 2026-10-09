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

/**
  @file storage/perfschema/table_setup_consumer_options.cc
  Percona: table SETUP_CONSUMER_OPTIONS (implementation).
*/

#include "storage/perfschema/table_setup_consumer_options.h"

#include <assert.h>
#include <stddef.h>

#include <iterator>

#include "m_string.h"
#include "mysql/strings/m_ctype.h"
#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "sql_string.h"
#include "storage/perfschema/pfs_latch_source.h"
#include "string_with_len.h"

static const LEX_CSTRING latch_source_granularity_values[] = {
    {STRING_WITH_LEN("NONE")}, {STRING_WITH_LEN("SOURCE")}, {nullptr, 0}};

static const row_setup_consumer_options all_setup_consumer_options_data[] = {
    {{STRING_WITH_LEN("latch_source_summary")},
     {STRING_WITH_LEN("granularity")},
     latch_source_granularity_values,
     &latch_source_granularity,
     update_latch_source_derived_flag}};

static constexpr uint COUNT_SETUP_CONSUMER_OPTIONS =
    std::size(all_setup_consumer_options_data);

THR_LOCK table_setup_consumer_options::m_table_lock;

Plugin_table table_setup_consumer_options::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "setup_consumer_options",
    /* Definition */
    "  CONSUMER VARCHAR(64) not null,\n"
    "  NAME VARCHAR(64) not null,\n"
    "  VALUE VARCHAR(64) not null,\n"
    "  PRIMARY KEY (CONSUMER, NAME) USING HASH\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_setup_consumer_options::m_share = {
    &pfs_updatable_acl,
    table_setup_consumer_options::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_setup_consumer_options::get_row_count,
    sizeof(PFS_simple_index), /* ref length */
    &m_table_lock,
    &m_table_def,
    false, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

bool PFS_index_setup_consumer_options::match(
    const row_setup_consumer_options *row) {
  if (m_fields >= 1) {
    if (!m_key_1.match(&row->m_consumer)) {
      return false;
    }
  }

  if (m_fields >= 2) {
    if (!m_key_2.match(&row->m_name)) {
      return false;
    }
  }

  return true;
}

PFS_engine_table *table_setup_consumer_options::create(
    PFS_engine_table_share *) {
  return new table_setup_consumer_options();
}

ha_rows table_setup_consumer_options::get_row_count() {
  return COUNT_SETUP_CONSUMER_OPTIONS;
}

table_setup_consumer_options::table_setup_consumer_options()
    : PFS_engine_table(&m_share, &m_pos),
      m_row(nullptr),
      m_pos(0),
      m_next_pos(0),
      m_opened_index(nullptr) {}

void table_setup_consumer_options::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

int table_setup_consumer_options::rnd_next() {
  m_pos.set_at(&m_next_pos);

  if (m_pos.m_index < COUNT_SETUP_CONSUMER_OPTIONS) {
    m_row = &all_setup_consumer_options_data[m_pos.m_index];
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  m_row = nullptr;
  return HA_ERR_END_OF_FILE;
}

int table_setup_consumer_options::rnd_pos(const void *pos) {
  set_position(pos);
  assert(m_pos.m_index < COUNT_SETUP_CONSUMER_OPTIONS);
  m_row = &all_setup_consumer_options_data[m_pos.m_index];
  return 0;
}

int table_setup_consumer_options::index_init(uint idx [[maybe_unused]], bool) {
  assert(idx == 0);
  auto *result = PFS_NEW(PFS_index_setup_consumer_options);
  m_opened_index = result;
  m_index = result;
  return 0;
}

int table_setup_consumer_options::index_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < COUNT_SETUP_CONSUMER_OPTIONS;
       m_pos.next()) {
    m_row = &all_setup_consumer_options_data[m_pos.m_index];

    if (m_opened_index->match(m_row)) {
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  m_row = nullptr;
  return HA_ERR_END_OF_FILE;
}

int table_setup_consumer_options::read_row_values(TABLE *table, unsigned char *,
                                                  Field **fields,
                                                  bool read_all) {
  Field *f;

  assert(m_row);

  /* Set the null bits */
  assert(table->s->null_bytes == 0);

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /* CONSUMER */
          set_field_varchar_utf8mb4(f, m_row->m_consumer.str,
                                    m_row->m_consumer.length);
          break;
        case 1: /* NAME */
          set_field_varchar_utf8mb4(f, m_row->m_name.str, m_row->m_name.length);
          break;
        case 2: /* VALUE */
        {
          const LEX_CSTRING &value = m_row->m_values[*m_row->m_value_ptr];
          set_field_varchar_utf8mb4(f, value.str, value.length);
          break;
        }
        default:
          assert(false);
      }
    }
  }

  return 0;
}

int table_setup_consumer_options::update_row_values(TABLE *table,
                                                    const unsigned char *,
                                                    unsigned char *,
                                                    Field **fields) {
  Field *f;
  char buffer[64];
  String text(buffer, sizeof(buffer), &my_charset_utf8mb4_bin);

  assert(m_row);

  for (; (f = *fields); fields++) {
    if (bitmap_is_set(table->write_set, f->field_index())) {
      switch (f->field_index()) {
        case 2: /* VALUE */
        {
          const String *input = get_field_varchar_utf8mb4(f, &text);

          ulong index = 0;
          for (; m_row->m_values[index].str != nullptr; index++) {
            const LEX_CSTRING &value = m_row->m_values[index];
            if (input->length() == value.length &&
                native_strncasecmp(input->ptr(), value.str, value.length) ==
                    0) {
              break;
            }
          }

          if (m_row->m_values[index].str == nullptr) {
            return HA_ERR_WRONG_COMMAND;
          }

          *m_row->m_value_ptr = index;
          break;
        }
        default:
          return HA_ERR_WRONG_COMMAND;
      }
    }
  }

  m_row->m_refresh();
  return 0;
}
