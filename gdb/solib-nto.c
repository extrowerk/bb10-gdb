/* solib-nto.c - SVR4 shared library extensions for QNX Neutrino

   Copyright (C) 2003-2019 Free Software Foundation, Inc.

   Contributed by QNX Software Systems Ltd.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include "elf/external.h"
#include "gdbcore.h"
#include "solib-svr4.h"

#include "solib-nto.h"

/* this section does not really exist in the core but BFD emulates it */
#define BFD_QNT_LINK_MAP  11
#define BFD_QNT_LINK_MAP_SEC_NAME ".qnx_link_map"

/* shortcut for endian fixing */
#define SWAP_UINT(var_ui, byte_order) \
  extract_unsigned_integer ((gdb_byte *)&var_ui, \
          sizeof (var_ui), byte_order);

/*
 * does an endian fix on the data returned from the BFD handler
 */
static struct qnx_link_map_64
swap_link_map (const gdb_byte *const lm)
{
  struct qnx_link_map_64 ret;

  struct link_map_offsets *lmo = nto_generic_svr4_fetch_link_map_offsets ();
  struct type *ptr_type = builtin_type (target_gdbarch ())->builtin_data_ptr;

  ret.l_addr = extract_typed_address (&lm[lmo->l_addr_offset],
                                                    ptr_type);
  ret.l_name = extract_typed_address (&lm[lmo->l_name_offset],
                                                    ptr_type);
  ret.l_ld = extract_typed_address (&lm[lmo->l_ld_offset],
                                                    ptr_type);
  ret.l_next = extract_typed_address (&lm[lmo->l_next_offset],
                                                    ptr_type);
  ret.l_prev = extract_typed_address (&lm[lmo->l_prev_offset],
                                                    ptr_type);
  ret.l_path = extract_typed_address (&lm[lmo->l_path_offset],
                                                    ptr_type);
  ret.l_refname = 0; /* unused */
  ret.l_loaded  = 0; /* unused */

  return ret;
}

/*
 * Just makes sure that the header fields have the right endian
 * It's debatable, if this is needed now(2019) as QNX only supports
 * LE platforms, but that may change back again if AARCH64be becomes
 * a common demand
 */
static struct qnx_linkmap_note_header
swap_header (const struct qnx_linkmap_note_header *const hp)
{
  enum bfd_endian byte_order = gdbarch_byte_order (target_gdbarch ());
  struct qnx_linkmap_note_header header;

  header.version      = SWAP_UINT (hp->version, byte_order);
  header.linkmapsz    = SWAP_UINT (hp->linkmapsz, byte_order);
  header.strtabsz     = SWAP_UINT (hp->strtabsz, byte_order);
  header.buildidtabsz = SWAP_UINT (hp->buildidtabsz, byte_order);
  return header;
}

/*
 * extract the buildid from the linkmap note
 */
static struct qnx_linkmap_note_buildid *
get_buildid_at (const struct qnx_linkmap_note_buildid *const buildidtab,
                const size_t buildidtabsz, const size_t index)
{
  enum bfd_endian byte_order = gdbarch_byte_order (target_gdbarch ());
  const struct qnx_linkmap_note_buildid *buildid;
  size_t n;

  for (n = 0, buildid = buildidtab;
      (uintptr_t)buildid < (uintptr_t)buildidtab + buildidtabsz; n++)
    {
      uint16_t descsz;
      uint16_t desctype;
      descsz = SWAP_UINT (buildid->descsz, byte_order);
      desctype = SWAP_UINT (buildid->desctype, byte_order);

      if (index == n)
        {
          struct qnx_linkmap_note_buildid *const ptr =
              (struct qnx_linkmap_note_buildid *) xcalloc (1, descsz + sizeof (descsz) + sizeof (desctype));
          ptr->descsz = descsz;
          ptr->desctype = desctype;
          memcpy (ptr->desc, buildid->desc, descsz);
          return ptr;
        }
      buildid = (struct qnx_linkmap_note_buildid *)((char *)buildid + sizeof(uint32_t) + descsz);
    }
  return NULL;
}

/*
 * fetch the list of shared objects from the linkmap_note
 *
 * this is needed as QNX historically has it's own way of referencing shared
 * libraries in the core file (see also: AUXV handling)
 */
struct so_list *
nto_solist_from_qnx_linkmap_note (void)
{
  struct so_list *head = NULL;
  struct so_list **tailp = NULL;
  const asection *qnt_link_map_sect;
  struct qnx_linkmap_note *qlmp;
  struct qnx_linkmap_note_header header;
  const char *strtab;
  const gdb_byte *lmtab;
  const struct qnx_linkmap_note_buildid *buildidtab;
  int n;
  size_t r_debug_sz = IS_64BIT() ? sizeof(struct qnx_r_debug_64) :
      sizeof(struct qnx_r_debug_32);
  size_t linkmap_sz = IS_64BIT() ? sizeof(struct qnx_link_map_64) :
      sizeof(struct qnx_link_map_32);

  nto_trace(0)("nto_solist_from_qnx_linkmap_note()\n");
  if (core_bfd == NULL)
    return NULL;

  /* Load link map from .qnx_link_map emulated in BFD */
  qnt_link_map_sect = bfd_get_section_by_name (core_bfd,
                 BFD_QNT_LINK_MAP_SEC_NAME);
  if (qnt_link_map_sect == NULL)
    {
      warning("Could not find %s in core!", BFD_QNT_LINK_MAP_SEC_NAME );
      return NULL;
    }

  qlmp = (struct qnx_linkmap_note *) xmalloc (bfd_get_section_size (qnt_link_map_sect));
  bfd_get_section_contents (core_bfd, (asection *)qnt_link_map_sect, qlmp, 0,
          bfd_get_section_size (qnt_link_map_sect));

  header = swap_header (&qlmp->header);

  strtab = (char *) &qlmp->data[qlmp->header.linkmapsz >> 2];
  lmtab = (gdb_byte *) &qlmp->data[(r_debug_sz + 3) >> 2];
  buildidtab = (struct qnx_linkmap_note_buildid *) &qlmp->data
    [((header.linkmapsz + 3) >> 2) + ((header.strtabsz + 3) >> 2)];

  for (n = 0; (n+1)*linkmap_sz <= header.linkmapsz; n++)
    {
      const struct qnx_link_map_64 lm = swap_link_map (lmtab+n*linkmap_sz);

      /* First static exe? */
      if (lm.l_next == lm.l_prev && lm.l_next == 1U)
        /* Artificial entry; skip it. */
        continue;

      if (lm.l_name < header.strtabsz)
        {
          const char *soname = &strtab[lm.l_name];
          const char *path = &strtab[lm.l_path];
          struct so_list *new_elem;
          int compressedpath = 0;
          struct qnx_linkmap_note_buildid *bldid;

          if (lm.l_prev == 0
              && (strcmp (soname, "PIE") == 0 || strcmp (soname, "EXE") == 0))
            /* Executable entry, skip it. */
            continue;
          lm_info_svr4 *li=(lm_info_svr4 *)xzalloc (sizeof (lm_info_svr4));
          new_elem = (struct so_list *) xzalloc (sizeof (struct so_list));
          new_elem->lm_info = li;
          li->lm_addr = 0;
          li->l_addr_p = 1; /* Do not calculate l_addr. */
          li->l_addr = lm.l_addr;
          /* On QNX we always set l_addr to image base address. */
          li->l_addr_inferior = lm.l_addr;
          li->l_ld = lm.l_ld;

          strncpy (new_elem->so_name, soname, sizeof (new_elem->so_name) - 1);
          new_elem->so_name [sizeof (new_elem->so_name) - 1] = 0;
          compressedpath = path[strlen(path)-1] == '/';
          snprintf (new_elem->so_original_name,
              sizeof (new_elem->so_original_name),
              "%s%s", path, compressedpath ? soname : "");

          new_elem->addr_low = lm.l_addr;
          new_elem->addr_high = lm.l_addr;

          bldid = get_buildid_at (buildidtab, header.buildidtabsz, n);
          if (bldid != NULL && bldid->descsz != 0)
            {
              new_elem->build_idsz = bldid->descsz;
              new_elem->build_id = (gdb_byte *) xcalloc (1, bldid->descsz);
              memcpy (new_elem->build_id, bldid->desc, bldid->descsz);
              xfree (bldid);
            }

          if (head == NULL)
            head = new_elem;
          else
            *tailp = new_elem;

          tailp = &new_elem->next;
        }
    }

  xfree (qlmp);

  return head;
}

/*
 * this compares the contents of a host address with the contents of the
 * target_address.
 */
int
nto_cmp_host_to_target_word (bfd *abfd, CORE_ADDR host_addr,
    CORE_ADDR target_addr)
{
  unsigned host_word, target_word;

  if ( ( bfd_seek(abfd, host_addr, SEEK_SET) != 0 )
      || ( bfd_bread ((gdb_byte*)&host_word, sizeof (host_word), abfd)
          != sizeof (host_word) ) )
    return -1;
  if (target_read_memory(target_addr, (gdb_byte*)&target_word,
       sizeof (target_word)))
    return -1;
  return (host_word-target_word);
}

/*
 * Check if the given shared object is valid for the debug target
 * this is done by comparing the build id's, making sure that both
 * denote the same code and offsets.
 *
 * returns 0 on success, 1 on failure, 2 on mismatch
 */
int
nto_so_validate (const struct so_list *const so)
{
  gdb_byte *build_id;
  size_t build_idsz;

  gdb_assert (so != NULL);

  nto_trace(0)("Validating %s\n", so->so_name );

  /* do we have a BFD? */
  if (so->abfd == NULL) {
    warning("No active BFD!");
    return 1;
  }

  /* is the BFD sane? */
  if (!bfd_check_format (so->abfd, bfd_object)
      || bfd_get_flavour (so->abfd) != bfd_target_elf_flavour
      || so->abfd->build_id == NULL) {
    warning("BFD mismatch!");
    return 1;
  }

  build_id = so->build_id;
  build_idsz = so->build_idsz;

  if (build_id == NULL)
    {
      nto_trace(0)("Fetch NOTE_GNU_BUILD_ID_NAME\n");
      /* Get build_id from NOTE_GNU_BUILD_ID_NAME section.
         This is a fallback mechanism for targets that do not
         implement TARGET_OBJECT_SOLIB_SVR4.  */

      const asection *const asec
          = bfd_get_section_by_name (so->abfd, NOTE_GNU_BUILD_ID_NAME);
      ULONGEST bfd_sect_size;

      if (asec == NULL)
        return 1;

      bfd_sect_size = bfd_get_section_size (asec);

      if ((asec->flags & SEC_LOAD) == SEC_LOAD
          && bfd_sect_size != 0
          && strcmp (bfd_section_name (asec->bfd, asec),
             NOTE_GNU_BUILD_ID_NAME) == 0)
        {
          const enum bfd_endian byte_order
              = gdbarch_byte_order (target_gdbarch ());
          Elf_External_Note *const note
              = (Elf_External_Note *const) xmalloc (bfd_sect_size);
          gdb_byte *const note_raw = (gdb_byte *const) note;
          struct cleanup *cleanups = make_cleanup (xfree, note);

          if (target_read_memory (bfd_get_section_vma (so->abfd, asec)
              + lm_addr_check (so, so->abfd),
              note_raw, bfd_sect_size) == 0)
            {
              build_idsz
                  = extract_unsigned_integer ((gdb_byte *) note->descsz,
                    sizeof (note->descsz), byte_order);

              if (build_idsz == so->abfd->build_id->size)
                {
                  const char gnu[] = "GNU";

                  if (memcmp (note->name, gnu, sizeof (gnu)) == 0)
                    {
                      ULONGEST namesz
                        = extract_unsigned_integer ((gdb_byte *) note->namesz,
                          sizeof (note->namesz),
                          byte_order);
                      CORE_ADDR build_id_offs;

                      /* Rounded to next 4 byte boundary.  */
                      namesz = (namesz + 3) & ~((ULONGEST) 3U);
                      build_id_offs = (sizeof (note->namesz)
                          + sizeof (note->descsz)
                          + sizeof (note->type) + namesz);
                      build_id = (gdb_byte *) xmalloc (build_idsz);
                      memcpy (build_id, note_raw + build_id_offs, build_idsz);
                    }
                }

              if (build_id == NULL)
                {
                  /* If we are here, it means target memory read succeeded
                     but note was not where it was expected according to the
                     abfd.  Allow the logic below to perform the check
                     with an impossible build-id and fail validation.  */
                  build_idsz = 0;
                  build_id = (gdb_byte*) xstrdup ("");
                }

            }
          do_cleanups (cleanups);
        }
    }

  if (build_id != NULL)
    {
      int match=1;
      if( ( so->abfd->build_id->size == build_idsz )
        && ( memcmp (build_id, so->abfd->build_id->data,
         so->abfd->build_id->size) == 0 ) ) {
        match=0;
    }
  else
    {
      /* This warning is being parsed by the IDE, the
       * format should not change without consultations with
       * IDE team.  */
      warning ("Host file %s does not match target file %s",
            so->so_name, so->so_original_name );
    }

    if (build_id != so->build_id)
      xfree (build_id);
    return match;
  }

  warning (_("Shared object \"%s\" could not be validated "
       "and will be ignored."), so->so_name);

  nto_trace(0)("No BuildID found!\n");
  return 1;
}
