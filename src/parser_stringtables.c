#include "parser_stringtables.h"
#include "alignof_wrapper.h"
#include "arena.h"
#include "demogobbler.h"
#include "demogobbler/bitstream.h"
#include "streams.h"
#include "writer.h"
#include <string.h>

static void write_stringtable_entry(bitwriter *writer, dg_stringtable_entry *entry) {
  bitwriter_write_cstring(writer, entry->name);
  bitwriter_write_bit(writer, entry->has_data);
  if (entry->has_data) {
    bitwriter_write_uint(writer, entry->size, 16);
    bitwriter_write_bitstream(writer, &entry->data);
  }
}

void dg_write_stringtables_parsed(writer *thisptr, dg_stringtables_parsed *message) {
  dg_write_byte(thisptr, message->orig.preamble.type);
  dg_write_int32(thisptr, message->orig.preamble.tick);
  if (thisptr->version.has_slot_in_preamble) {
    dg_write_byte(thisptr, message->orig.preamble.slot);
  }

  thisptr->bitwriter.bitoffset = 0;
#ifdef GROUND_TRUTH_CHECK
  thisptr->bitwriter.truth_data = message->orig.data;
  thisptr->bitwriter.truth_data_offset = 0;
  thisptr->bitwriter.truth_size_bits = message->orig.size_bytes * 8;
#endif

  bitwriter_write_uint(&thisptr->bitwriter, message->tables_count, 8);
  for (uint8_t i = 0; i < message->tables_count; ++i) {
    dg_stringtable *table = message->tables + i;
    bitwriter_write_cstring(&thisptr->bitwriter, table->table_name);
    bitwriter_write_uint(&thisptr->bitwriter, table->entries_count, 16);

    for (uint16_t u = 0; u < table->entries_count; ++u) {
      write_stringtable_entry(&thisptr->bitwriter, table->entries + u);
    }

    bitwriter_write_bit(&thisptr->bitwriter, table->has_classes);

    if (table->has_classes) {
      bitwriter_write_uint(&thisptr->bitwriter, table->classes_count, 16);
      for (uint16_t u = 0; u < table->classes_count; ++u) {
        write_stringtable_entry(&thisptr->bitwriter, table->classes + u);
      }
    }
  }

  if (thisptr->bitwriter.bitoffset % 8 != 0) {
    if (thisptr->bitwriter.bitoffset == message->leftover_bits.bitoffset) {
      bitwriter_write_bitstream(&thisptr->bitwriter, &message->leftover_bits);
    }
  }

  uint32_t bytes = thisptr->bitwriter.bitoffset / 8;
  dg_write_int32(thisptr, thisptr->bitwriter.bitoffset / 8);
  dg_write_data(thisptr, thisptr->bitwriter.ptr, bytes);
}

static const char *read_string(dg_bitstream *stream, dg_arena *memory_arena) {
  char BUFFER[1024];
  uint32_t bytes = bitstream_read_cstring(stream, BUFFER, 1024);
  char *dest = dg_arena_allocate(memory_arena, bytes, 1);
  memcpy(dest, BUFFER, bytes);
  return dest;
}

static void read_stringtable_entry(dg_stringtable_entry *entry, dg_bitstream *stream,
                                   stringtable_parse_args args) {
  entry->name = read_string(stream, args.memory_arena);
  entry->has_data = bitstream_read_bit(stream);
  if (entry->has_data) {
    entry->size = bitstream_read_uint(stream, 16);
    entry->data = bitstream_fork_and_advance(stream, entry->size * 8);
  }
}

dg_parse_result dg_parse_stringtables(dg_stringtables_parsed *message,
                                      stringtable_parse_args args) {
  dg_parse_result result;
  memset(message, 0, sizeof(*message));
  memset(&result, 0, sizeof(result));
  message->orig = *args.message;

  dg_bitstream stream = bitstream_create(args.message->data, args.message->size_bytes * 8);
  message->tables_count = bitstream_read_uint(&stream, 8);
  message->tables = dg_arena_allocate(
      args.memory_arena, message->tables_count * sizeof(dg_stringtable), alignof(dg_stringtable));
  memset(message->tables, 0, message->tables_count * sizeof(dg_stringtable));

  for (uint8_t i = 0; i < message->tables_count; ++i) {
    dg_stringtable *table = message->tables + i;
    table->table_name = read_string(&stream, args.memory_arena);
    table->entries_count = bitstream_read_uint(&stream, 16);
    table->entries =
        dg_arena_allocate(args.memory_arena, table->entries_count * sizeof(dg_stringtable_entry),
                          alignof(dg_stringtable_entry));
    memset(table->entries, 0, table->entries_count * sizeof(dg_stringtable_entry));
    for (uint16_t u = 0; u < table->entries_count; ++u) {
      read_stringtable_entry(table->entries + u, &stream, args);
    }

    table->has_classes = bitstream_read_bit(&stream);
    if (table->has_classes) {
      table->classes_count = bitstream_read_uint(&stream, 16);
      table->classes =
          dg_arena_allocate(args.memory_arena, table->classes_count * sizeof(dg_stringtable_entry),
                            alignof(dg_stringtable_entry));
      for (uint16_t u = 0; u < table->classes_count; ++u) {
        read_stringtable_entry(table->classes + u, &stream, args);
      }
    }
  }

  unsigned bits_left = dg_bitstream_bits_left(&stream);

  if (bits_left != 0) {
    message->leftover_bits = bitstream_fork_and_advance(&stream, bits_left);
  }

  if (stream.overflow) {
    result.error = true;
    result.error_message = "Overflowed during dg_stringtable parsing";
  }

  return result;
}

void dg_parser_parse_stringtables(parser *thisptr, dg_stringtables *input) {
  dg_stringtables_parsed out;
  stringtable_parse_args args;
  memset(&out, 0, sizeof(out));
  memset(&args, 0, sizeof(args));

  args.memory_arena = &thisptr->temp_arena;
  args.message = input;

  dg_parse_result result = dg_parse_stringtables(&out, args);

  if (!result.error || thisptr->m_settings.stringtables_parsed_handler) {
    thisptr->m_settings.stringtables_parsed_handler(&thisptr->state, &out);
  } else if (result.error) {
    thisptr->error = true;
    thisptr->error_message = result.error_message;
  }
}
