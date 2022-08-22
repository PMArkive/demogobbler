#include "parser.h"
#include "demogobbler.h"
#include "filereader.h"
#include "packettypes.h"
#include "parser_datatables.h"
#include "parser_netmessages.h"
#include "utils.h"
#include "version_utils.h"
#include <stddef.h>
#include <string.h>

#define thisreader &thisptr->m_reader
#define thisallocator &thisptr->allocator

void _parser_mainloop(parser *thisptr);
void _parse_header(parser *thisptr);
void _parse_consolecmd(parser *thisptr);
void _parse_customdata(parser *thisptr);
void _parse_datatables(parser *thisptr);
void _parse_packet(parser *thisptr, enum demogobbler_type type);
void _parse_stop(parser *thisptr);
void _parse_stringtables(parser *thisptr, int32_t type);
void _parse_synctick(parser *thisptr);
void _parse_usercmd(parser *thisptr);
bool _parse_anymessage(parser *thisptr);

void parser_init(parser *thisptr, demogobbler_settings *settings) {
  memset(thisptr, 0, sizeof(*thisptr));
  thisptr->state.client_state = settings->client_state;
  thisptr->m_settings = *settings;

  const size_t INITIAL_SIZE = 1 << 15;
  // Does lazy allocation, only allocates stuff if requested
  thisptr->memory_arena = demogobbler_arena_create(INITIAL_SIZE); 
}

void parser_parse(parser *thisptr, void *stream, input_interface input) {
  if (stream) {
    enum { FILE_BUFFER_SIZE = 1 << 15 };
    uint64_t buffer[FILE_BUFFER_SIZE / sizeof(uint64_t)];
    filereader_init(thisreader, buffer, sizeof(buffer), stream, input);
    _parse_header(thisptr);
    _parser_mainloop(thisptr);
  }
}

void parser_update_l4d2_version(parser *thisptr, int l4d2_version) {
  thisptr->demo_version.l4d2_version = l4d2_version;
  thisptr->demo_version.l4d2_version_finalized = true;
  version_update_build_info(&thisptr->demo_version);
  if (thisptr->m_settings.demo_version_handler) {
    thisptr->m_settings.demo_version_handler(&thisptr->state, thisptr->demo_version);
  }
}

size_t _parser_read_length(parser *thisptr) {
  int32_t result = filereader_readint32(thisreader);
  int32_t max_len = 1 << 25; // more than 32 megabytes is probably an error
  if (result < 0 || result > max_len) {
    thisptr->error = true;
    thisptr->error_message = "Length was invalid\n";
    return 0;
  } else {
    return result;
  }
}

void _parse_header(parser *thisptr) {
  demogobbler_header header;
  filereader_readdata(thisreader, header.ID, 8);
  header.demo_protocol = filereader_readint32(thisreader);
  header.net_protocol = filereader_readint32(thisreader);

  filereader_readdata(thisreader, header.server_name, 260);
  filereader_readdata(thisreader, header.client_name, 260);
  filereader_readdata(thisreader, header.map_name, 260);
  filereader_readdata(thisreader, header.game_directory, 260);
  header.seconds = filereader_readfloat(thisreader);
  header.tick_count = filereader_readint32(thisreader);
  header.frame_count = filereader_readint32(thisreader);
  header.signon_length = filereader_readint32(thisreader);

  // Add null terminators if they are missing
  header.ID[7] = header.server_name[259] = header.client_name[259] = header.map_name[259] =
      header.game_directory[259] = '\0';

  thisptr->demo_version = demogobbler_get_demo_version(&header);

  if (thisptr->m_settings.demo_version_handler) {
    thisptr->m_settings.demo_version_handler(&thisptr->state, thisptr->demo_version);
  }

  if (thisptr->m_settings.header_handler) {
    thisptr->m_settings.header_handler(&thisptr->state, &header);
  }
}

bool _parse_anymessage(parser *thisptr) {
  uint8_t type = filereader_readbyte(thisreader);

  switch (type) {
  case demogobbler_type_consolecmd:
    _parse_consolecmd(thisptr);
    break;
  case demogobbler_type_datatables:
    _parse_datatables(thisptr);
    break;
  case demogobbler_type_packet:
    _parse_packet(thisptr, type);
    break;
  case demogobbler_type_signon:
    _parse_packet(thisptr, type);
    break;
  case demogobbler_type_stop:
    _parse_stop(thisptr);
    break;
  case demogobbler_type_synctick:
    _parse_synctick(thisptr);
    break;
  case demogobbler_type_usercmd:
    _parse_usercmd(thisptr);
    break;
  case 8:
    if (thisptr->demo_version.demo_protocol < 4) {
      _parse_stringtables(thisptr, 8);
    } else {
      _parse_customdata(thisptr);
    }
    break;
  case 9:
    _parse_stringtables(thisptr, 9);
    break;
  default:
    thisptr->error = true;
    thisptr->error_message = "Invalid message type";
    break;
  }

  return type != demogobbler_type_stop && !thisptr->m_reader.eof &&
         !thisptr->error; // Return false when done parsing demo, or when at eof
}

#define PARSE_PREAMBLE()                                                                           \
  message.preamble.tick = filereader_readint32(thisreader);                                        \
  if (thisptr->demo_version.has_slot_in_preamble)                                                  \
    message.preamble.slot = filereader_readbyte(thisreader);

void _parser_mainloop(parser *thisptr) {
  // Add check if the only thing we care about is the header

  demogobbler_settings *settings = &thisptr->m_settings;
  bool should_parse = false;

#define NULL_CHECK(name)                                                                           \
  if (settings->name##_handler)                                                                    \
    should_parse = true;

  NULL_CHECK(packet_net_message);
  NULL_CHECK(consolecmd);
  NULL_CHECK(customdata);
  NULL_CHECK(datatables);
  NULL_CHECK(datatables_parsed);
  NULL_CHECK(packet);
  NULL_CHECK(synctick);
  NULL_CHECK(stop);
  NULL_CHECK(stringtables);
  NULL_CHECK(usercmd);

  if (settings->entity_state_init_handler || settings->store_ents) {
    settings->store_ents = true; // Entity state init handler => we should store ents
    should_parse = true;
  }

#undef NULL_CHECK

  if (!should_parse)
    return;

  enum { STACK_SIZE = 1 << 13 };
  uint64_t buffer[STACK_SIZE / sizeof(uint64_t)];
  allocator_init(&thisptr->allocator, buffer, sizeof(buffer));

  while (_parse_anymessage(thisptr))
    ;
  demogobbler_arena_free(&thisptr->memory_arena);
  free(thisptr->state.entity_state.sendtables);
}

#define READ_MESSAGE_DATA()                                                                        \
  {                                                                                                \
    size_t read_bytes = filereader_readdata(thisreader, block.address, message.size_bytes);        \
    if (read_bytes != message.size_bytes) {                                                        \
      thisptr->error = true;                                                                       \
      thisptr->error_message = "Message could not be read fully, reached end of file.";            \
    }                                                                                              \
    message.data = block.address;                                                                  \
  }

void _parse_consolecmd(parser *thisptr) {
  demogobbler_consolecmd message;
  message.preamble.type = message.preamble.converted_type = demogobbler_type_consolecmd;
  PARSE_PREAMBLE();
  message.size_bytes = _parser_read_length(thisptr);

  if (message.size_bytes > 0) {
    ;
    if (thisptr->m_settings.consolecmd_handler) {
      blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
      READ_MESSAGE_DATA();

      if (!thisptr->error) {
        message.data[message.size_bytes - 1] = '\0'; // Add null terminator in-case malformed data
        thisptr->m_settings.consolecmd_handler(&thisptr->state, &message);
      }

      allocator_dealloc(thisallocator, block);

    } else {
      filereader_skipbytes(thisreader, message.size_bytes);
    }
  } else {
    thisptr->error = true;
    thisptr->error_message = "Invalid consolecommand length";
  }
}

void _parse_customdata(parser *thisptr) {
  demogobbler_customdata message;
  message.preamble.type = message.preamble.converted_type = demogobbler_type_customdata;
  PARSE_PREAMBLE();

  message.unknown = filereader_readint32(thisreader);
  message.size_bytes = _parser_read_length(thisptr);

  if (thisptr->m_settings.customdata_handler && message.size_bytes > 0) {
    blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
    READ_MESSAGE_DATA();
    if (!thisptr->error) {
      thisptr->m_settings.customdata_handler(&thisptr->state, &message);
    }
    allocator_dealloc(thisallocator, block);
  } else {
    filereader_skipbytes(thisreader, message.size_bytes);
  }
}

void _parse_datatables(parser *thisptr) {
  demogobbler_datatables message;
  message.preamble.type = message.preamble.converted_type = demogobbler_type_datatables;
  PARSE_PREAMBLE();

  message.size_bytes = _parser_read_length(thisptr);

  bool has_datatable_handler = thisptr->m_settings.datatables_handler ||
                               thisptr->m_settings.datatables_parsed_handler ||
                               thisptr->m_settings.store_ents;

  if (has_datatable_handler && message.size_bytes > 0) {
    blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
    READ_MESSAGE_DATA();
    if (!thisptr->error) {

      if (thisptr->m_settings.datatables_handler)
        thisptr->m_settings.datatables_handler(&thisptr->state, &message);

      if (thisptr->m_settings.datatables_parsed_handler || thisptr->m_settings.store_ents)
        parse_datatables(thisptr, &message);
    }
    allocator_dealloc(thisallocator, block);
  } else {
    filereader_skipbytes(thisreader, message.size_bytes);
  }
}

static void _parse_cmdinfo(parser *thisptr, demogobbler_packet *packet, size_t i) {
  if (thisptr->m_settings.packet_handler) {
    filereader_readdata(thisreader, packet->cmdinfo_raw[i].data,
                        sizeof(packet->cmdinfo_raw[i].data));
  } else {
    filereader_skipbytes(thisreader, sizeof(struct demogobbler_cmdinfo_raw));
  }
}

void _parse_packet(parser *thisptr, enum demogobbler_type type) {
  demogobbler_packet message;
  message.preamble.type = message.preamble.converted_type = type;
  PARSE_PREAMBLE();
  message.cmdinfo_size = thisptr->demo_version.cmdinfo_size;

  for (int i = 0; i < message.cmdinfo_size; ++i) {
    _parse_cmdinfo(thisptr, &message, i);
  }

  message.in_sequence = filereader_readint32(thisreader);
  message.out_sequence = filereader_readint32(thisreader);
  message.size_bytes = _parser_read_length(thisptr);

  if ((thisptr->m_settings.packet_handler || thisptr->m_settings.packet_net_message_handler) &&
      message.size_bytes > 0) {
    blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
    READ_MESSAGE_DATA();
    if (!thisptr->error) {

      if (thisptr->m_settings.packet_handler) {
        thisptr->m_settings.packet_handler(&thisptr->state, &message);
      }

      if (thisptr->m_settings.packet_net_message_handler) {
        parse_netmessages(thisptr, block.address, message.size_bytes);
      }
    }

    allocator_dealloc(thisallocator, block);
  } else {
    filereader_skipbytes(thisreader, message.size_bytes);
  }
}

void _parse_stop(parser *thisptr) {

  if (thisptr->m_settings.stop_handler) {
    demogobbler_stop message;

    const int bytes_per_read = 4096;
    size_t bytes = 0;
    size_t bytesReadIt;
    blk block = allocator_alloc(&thisptr->allocator, bytes_per_read);

    do {
      if (bytes + bytes_per_read > block.size) {
        block = allocator_realloc(&thisptr->allocator, block, bytes + bytes_per_read);
      }

      bytesReadIt =
          filereader_readdata(thisreader, (uint8_t *)block.address + bytes, bytes_per_read);
      bytes += bytesReadIt;
    } while (bytesReadIt == bytes_per_read);
    message.size_bytes = bytes;
    message.data = block.address;

    thisptr->m_settings.stop_handler(&thisptr->state, &message);
    allocator_dealloc(thisallocator, block);
  }
}

void _parse_stringtables(parser *thisptr, int32_t type) {
  demogobbler_stringtables message;
  message.preamble.type = type;
  message.preamble.converted_type = demogobbler_type_stringtables;
  PARSE_PREAMBLE();
  message.size_bytes = _parser_read_length(thisptr);

  if (thisptr->m_settings.stringtables_handler && message.size_bytes > 0) {
    blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
    READ_MESSAGE_DATA();
    if (!thisptr->error) {
      thisptr->m_settings.stringtables_handler(&thisptr->state, &message);
    }
    allocator_dealloc(thisallocator, block);
  } else {
    filereader_skipbytes(thisreader, message.size_bytes);
  }
}

void _parse_synctick(parser *thisptr) {
  demogobbler_synctick message;
  message.preamble.type = message.preamble.converted_type = demogobbler_type_synctick;
  PARSE_PREAMBLE();

  if (thisptr->m_settings.synctick_handler) {
    thisptr->m_settings.synctick_handler(&thisptr->state, &message);
  }
}

void _parse_usercmd(parser *thisptr) {
  demogobbler_usercmd message;
  message.preamble.type = message.preamble.converted_type = demogobbler_type_usercmd;
  PARSE_PREAMBLE();

  message.cmd = filereader_readint32(thisreader);
  message.size_bytes = filereader_readint32(thisreader);

  if (thisptr->m_settings.usercmd_handler && message.size_bytes > 0) {

    blk block = allocator_alloc(&thisptr->allocator, message.size_bytes);
    READ_MESSAGE_DATA();
    if (!thisptr->error) {
      thisptr->m_settings.usercmd_handler(&thisptr->state, &message);
    }
    allocator_dealloc(thisallocator, block);
  } else {
    filereader_skipbytes(thisreader, message.size_bytes);
  }
}
