/* sd2iec - SD/MMC to Commodore serial bus interface/controller
   Copyright (C) 2007  Ingo Korb <ingo@akana.de>

   Inspiration and low-level SD/MMC access based on code from MMC2IEC
     by Lars Pontoppidan et al., see sdcard.c|h and config.h.

   FAT filesystem access based on code from ChaN, see tff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   
   fileops.c: Generic file operations

*/

#include <avr/pgmspace.h>
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "errormsg.h"
#include "buffers.h"
#include "uart.h"
#include "doscmd.h"
#include "wrapops.h"
#include "fileops.h"

/* ------------------------------------------------------------------------- */
/*  Some constants used for directory generation                             */
/* ------------------------------------------------------------------------- */

#define HEADER_OFFSET_NAME 
#define HEADER_OFFSET_ID   26

/* NOTE: I wonder if RLE-packing would save space in flash? */
const PROGMEM uint8_t dirheader[] = {
  1, 4,                            /* BASIC start address */
  1, 1,                            /* next line pointer */
  0, 0,                            /* line number 0 */
  0x12, 0x22,                      /* Reverse on, quote */
  ' ',' ',' ',' ',' ',' ',' ',' ', /* 16 spaces as the disk name */
  ' ',' ',' ',' ',' ',' ',' ',' ', /* will be overwritten if needed */
  0x22,0x20,                       /* quote, space */
  'I','K',' ','2','A',             /* id IK, shift-space, dosmarker 2A */
  00                               /* line-end marker */
};

const PROGMEM uint8_t dirfooter[] = {
  1, 1, /* next line pointer */
  0, 0, /* number of free blocks (to be filled later */
  'B','L','O','C','K','S',' ','F','R','E','E','.',
  0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, /* Filler and end marker */
  0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00
};

const PROGMEM uint8_t filetypes[] = {
  'D','E','L', // 0
  'S','E','Q', // 1
  'P','R','G', // 2
  'U','S','R', // 3
  'R','E','L', // 4
  'C','B','M', // 5
  'D','I','R'  // 6
};

/* Pointer to the currently active fileops structure */
const fileops_t *fop;

/* ------------------------------------------------------------------------- */
/*  Utility functions                                                        */
/* ------------------------------------------------------------------------- */

/* Zero-terminate a padded commodore name */
char *dent2str(struct cbmdirent *dent) {
  uint8_t i;

  for (i=0;i<CBM_NAME_LENGTH;i++) {
    if (dent->name[i] == 0xa0) {
      dent->name[i] = 0;
      return (char *)dent->name;
    }
  }
  dent->name[CBM_NAME_LENGTH] = 0;
  return (char *)dent->name;
}

/* Add a single directory entry in 15x1 format to the end of buf */
static void addentry(struct cbmdirent *dent, buffer_t *buf) {
  uint8_t i;
  uint8_t *data;

  data = buf->data + buf->length;

  /* Clear the line */
  memset(data, ' ', 31);
  /* Line end marker */
  data[31] = 0;

  /* Next line pointer, 1571-compatible =) */
  *data++ = 1;
  *data++ = 1;
  
  *data++ = dent->blocksize & 0xff;
  *data++ = dent->blocksize >> 8;
  
  /* Filler before file name */
  if (dent->blocksize < 1000)
    *data++;
  if (dent->blocksize < 100)
    *data++;
  if (dent->blocksize < 10)
    *data++;
  *data++ = '"';

  /* copy and adjust the filename - C783 */
  memcpy(data, dent->name, CBM_NAME_LENGTH);
  for (i=0;i<=CBM_NAME_LENGTH;i++)
    if (dent->name[i] == 0x22 || dent->name[i] == 0xa0 || i == 16) {
      data[i] = '"';
      while (i<=CBM_NAME_LENGTH)
	data[i++] &= 0x7f;
    }

  /* Skip name and final quote */
  data += CBM_NAME_LENGTH+1;

  if (dent->typeflags & FLAG_SPLAT)
    *data = '*';

  /* File type */
  memcpy_P(data+1, filetypes + TYPE_LENGTH * (dent->typeflags & TYPE_MASK), TYPE_LENGTH);

  /* RO marker */
  if (dent->typeflags & FLAG_RO)
    data[4] = '<';

  /* Extension: Hidden marker */
  if (dent->typeflags & FLAG_HIDDEN)
    data[5] = 'H';

  buf->length += 32;
}

/* Match a pattern against a CBM-padded filename */
/* Returns 1 if matching */
static uint8_t match_name(char *matchstr, uint8_t *filename) {
  uint8_t i = 0;

  while (filename[i] != 0xa0 && i < CBM_NAME_LENGTH) {
    switch (*matchstr) {
    case '?':
      i++;
      matchstr++;
      break;

    case '*':
      return 1;

    default:
      if (filename[i++] != *matchstr++)
	return 0;
      break;
    }
  }
  if (*matchstr && *matchstr != '*')
    return 0;
  else
    return 1;
}

/* Get next matching dirent */
/* Return values:           */
/*  -1: no more matches     */
/*   0: match found         */
/*   1: error               */
int8_t next_match(DIR *dh, char *matchstr, uint8_t type, struct cbmdirent *dent) {
  int8_t res;
  while (1) {
    res = readdir(dh, dent);
    if (res == 0) {
      /* Skip if the type doesn't match */
      if ((type & TYPE_MASK) &&
	  (dent->typeflags & TYPE_MASK) != (type & TYPE_MASK))
	continue;
      
      /* Skip hidden files */
      if ((dent->typeflags & FLAG_HIDDEN) &&
	  !(type & FLAG_HIDDEN))
	continue;

      /* Skip if the name doesn't match */
      if (matchstr &&
	  !match_name(matchstr, dent->name))
	continue;
    }

    return res;
  }
}

/* ------------------------------------------------------------------------- */
/*  Callbacks                                                                */
/* ------------------------------------------------------------------------- */

/* Generic cleanup-callback that just frees the buffer */
uint8_t generic_cleanup(buffer_t *buf) {
  free_buffer(buf);
  return 0;
}

/* Generate the final directory buffer with the BLOCKS FREE message */
static uint8_t dir_footer(buffer_t *buf) {
  uint16_t blocks;

  /* Copy the "BLOCKS FREE" message */
  memcpy_P(buf->data, dirfooter, sizeof(dirfooter));

  blocks = disk_free();
  buf->data[2] = blocks & 0xff;
  buf->data[3] = blocks >> 8;

  buf->position = 0;
  buf->length   = 31;
  buf->sendeoi  = 1;

  return 0;
}

/* Fill the buffer with one new directory entry */
static uint8_t dir_refill(buffer_t *buf) {
  struct cbmdirent dent;

  uart_putc('+');

  buf->position = 0;
  buf->length   = 0;

  switch (next_match(&buf->pvt.dir.dh, buf->pvt.dir.matchstr,
		     buf->pvt.dir.filetype, &dent)) {
  case 0:
    addentry(&dent, buf);
    buf->length--;
    return 0;
      
  case -1:
    return dir_footer(buf);
    
  default:
    free_buffer(buf);
    return 1;
  }
}

/* Prepare for directory reading and create the header */
static void load_directory(uint8_t secondary) {
  buffer_t *buf;
  uint8_t i;

  buf = alloc_buffer();
  if (!buf)
    return;

  buf->pvt.dir.filetype = 0;
  if (command_length > 2) {
    /* Parse the name pattern */
    char *name;
    
    parse_path((char *) command_buffer+1, (char *) command_buffer, &name);

    if (opendir(&buf->pvt.dir.dh, (char *) command_buffer)) {
      free_buffer(buf);
      return;
    }

    buf->pvt.dir.matchstr = name;

    /* Check for a filetype match */
    name = strchr(name, '=');
    if (name != NULL) {
      *name++ = 0;
      switch (*name++) {
      case 'S':
	buf->pvt.dir.filetype = TYPE_SEQ;
	break;

      case 'P':
	buf->pvt.dir.filetype = TYPE_PRG;
	break;

      case 'U':
	buf->pvt.dir.filetype = TYPE_USR;
	break;

      case 'R':
	buf->pvt.dir.filetype = TYPE_REL;
	break;

      case 'C': /* This is guessed, not verified */
	buf->pvt.dir.filetype = TYPE_CBM;
	break;

      case 'B': /* CMD compatibility */
      case 'D': /* Specifying DEL matches everything anyway */
	buf->pvt.dir.filetype = TYPE_DIR;
	break;

      case 'H': /* Extension: Also show hidden files */
	buf->pvt.dir.filetype = FLAG_HIDDEN;
	break;

      default:
	buf->pvt.dir.filetype = 0;
	break;
      }
    }
  } else {
    if (opendir(&buf->pvt.dir.dh,"")) {
      free_buffer(buf);
      return;
    }
    buf->pvt.dir.matchstr = NULL;
  }

  buf->secondary = secondary;
  buf->read      = 1;
  buf->write     = 0;
  buf->cleanup   = generic_cleanup;
  buf->position  = 0;
  buf->length    = 31;
  buf->sendeoi   = 0;

  /* copy static header to start of buffer */
  memcpy_P(buf->data, dirheader, sizeof(dirheader));

  /* read volume name */
  if (disk_label((char *) (buf->data+8))) {
    free_buffer(buf);
    return;
  }

  /* read id */
  if (disk_id((char *) (buf->data+HEADER_OFFSET_ID))) {
    free_buffer(buf);
    return;
  }

  /* Let the refill callback handly everything else */
  buf->refill = dir_refill;

  return;
}

/* ------------------------------------------------------------------------- */
/*  External interface for the various operations                            */
/* ------------------------------------------------------------------------- */

/* Open something */
void file_open(uint8_t secondary) {
  buffer_t *buf;
  uint8_t i;

  /* Assume everything will go well unless proven otherwise */
  set_error(ERROR_OK,0,0);

  /* Empty name? */
  if (command_length == 0) {
    set_error(ERROR_SYNTAX_NONAME,0,0);
    return;
  }

  command_buffer[command_length] = 0;

  /* Load directory? */
  if (command_buffer[0] == '$') {
    // FIXME: Secondary != 0? Compare D7F7-D7FF
    load_directory(secondary);
    return;
  }

  /* Parse type+mode suffixes */
  char *ptr = (char *) command_buffer;
  enum open_modes mode = OPEN_READ;
  uint8_t filetype = TYPE_DEL;

  while (*ptr && (ptr = strchr(ptr, ','))) {
    *ptr = 0;
    ptr++;
    switch (*ptr) {
    case 0:
      break;

    case 'R': /* Read */
      mode = OPEN_READ;
      break;

    case 'W': /* Write */
      mode = OPEN_WRITE;
      break;

    case 'A': /* Append */
      mode = OPEN_APPEND;
      break;

    case 'M': /* Modify */
      mode = OPEN_MODIFY;
      break;

    case 'D': /* DEL */
      filetype = TYPE_DEL;
      break;

    case 'S': /* SEQ */
      filetype = TYPE_SEQ;
      break;

    case 'P': /* PRG */
      filetype = TYPE_PRG;
      break;

    case 'U': /* USR */
      filetype = TYPE_USR;
      break;

    case 'L': /* REL */
      filetype = TYPE_REL;
      // FIXME: REL wird nach dem L anders geparst!
      break;
    }
  }

  /* Force mode for secondaries 0/1 */
  switch (secondary) {
  case 0:
    mode = OPEN_READ;
    if (filetype == TYPE_DEL)
      filetype = TYPE_PRG;
    break;

  case 1:
    mode = OPEN_WRITE;
    if (filetype == TYPE_DEL)
      filetype = TYPE_PRG;
    break;

  default:
    if (filetype == TYPE_DEL)
      filetype = TYPE_SEQ;
  }

  /* Parse path+drive numbers */
  char *cbuf = (char *) command_buffer;
  char *fname;
  int8_t res;
  struct cbmdirent dent;

  /* Check for rewrite marker */
  if (command_buffer[0] == '@')
    cbuf = (char *)command_buffer+1;
  else
    cbuf = (char *)command_buffer;

  parse_path(cbuf, (char *) command_buffer, &fname);

  /* Filename matching */
  if (opendir(&matchdh, (char *) command_buffer))
    return;

  res = next_match(&matchdh, fname, FLAG_HIDDEN, &dent);
  if (res > 0)
    return;

  if (mode == OPEN_WRITE) {
    /* Normales Schreiben: Nicht matchen (oder mit File Exists abbrechen) */
    /* Rewrite: Matchen. Wenn gefunden loeschen, wenn nicht egal */
    if (cbuf != (char *) command_buffer && res == 0) {
      /* Rewrite existing file: Delete the old one */
      /* This is safe. If there is no buffer available, delete will fail. */
      if (file_delete((char *) command_buffer, fname) == 255)
	return;
    } else {
      /* Normal write or non-existing rewrite */
      /* Doesn't exist: Copy name to dent */
      strcpy((char *)dent.name, fname);
    }
  } else
    if (res != 0) {
      /* File not found */
      set_error(ERROR_FILE_NOT_FOUND,0,0);
      return;
    }

  fname = dent2str(&dent);

  /* Grab a buffer */
  buf = alloc_buffer();
  if (!buf)
    return;

  buf->secondary = secondary;

  switch (mode) {
  case OPEN_MODIFY:
  case OPEN_READ:
    /* Modify is the same as read, but allows reading *ed files.        */
    /* FAT doesn't have anything equivalent, so both are mapped to READ */
    open_read((char *) command_buffer, fname, buf);
    break;

  case OPEN_WRITE:
  case OPEN_APPEND:
    open_write((char *) command_buffer, fname, buf, (mode == OPEN_APPEND));
    break;
  }
}
