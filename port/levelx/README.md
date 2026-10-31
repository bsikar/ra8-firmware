# port/levelx -- LevelX bridge into ra8_fs

This directory carries the adapter that puts the first-party ``ra8_fs``
filesystem on top of the LevelX wear-levelling layer, and the port shim that
drives LevelX over the EK-RA8D2 on-board ISSI IS25LX512M octal-SPI NOR flash
via ``ra8_xspi_flash_*``. Two pieces live here:

- ``lx_fs_backend.c`` / ``lx_fs_backend.h``: exposes an open ``LX_NOR_FLASH``
  as an ::ra8_fs_backend_t (``read_block`` / ``write_block`` /
  ``get_capacity``) through ``lx_fs_backend_bind()``, so ``ra8_fs_format`` and
  ``ra8_fs_mount`` see a plain block device.
- ``lx_nor_driver_ra8_xspi.c`` / ``.h``: the LevelX NOR driver bound to the
  on-board flash.

## Stack layout

```
ra8_fs (FAT, ra8_fs_mount)
    |
    v
lx_fs_backend (port/levelx: read_block / write_block / get_capacity)
    |
    v
LevelX (lx_nor_flash_sector_read / write)
    |
    v
lx_nor_driver_ra8_xspi (port/levelx/src/lx_nor_driver_ra8_xspi.c)
    |
    v
ra8_xspi_flash_read / program / erase_sector
    |
    v
IS25LX512M octal-SPI NOR (EK-RA8D2 on-board chip)
```

## Build modes

``cmake/levelx_standalone.cmake`` exposes ``RA8_USE_LEVELX_STANDALONE``: the
real vendored LevelX NOR core compiled with ``LX_STANDALONE_ENABLE``, no
ThreadX. ``RA8_USE_LEVELX`` keeps the module-kernel build where LevelX runs on
top of ThreadX. The two options are mutually exclusive and a standalone build
requires the pinned vendored LevelX package to be present; the justfile forces
``-DRA8_USE_LEVELX_STANDALONE=ON``.

## Geometry and semantics

- LevelX exposes 512-byte logical sectors (``LX_NOR_SECTOR_SIZE *
  sizeof(ULONG)``), which is also the smallest sector size ``ra8_fs``
  supports, so each backend block maps 1:1 onto one LevelX call.
- Reported capacity is ``total_blocks * (physical_sectors_per_block - 1)``:
  LevelX's own per-block sector count (already excluding its one metadata
  sector per block) minus one more sector per block as wear-levelling
  headroom. This is the same margin the retired FileX adapter advertised to
  ``fx_media_format``.
- A logical sector LevelX has never mapped is erased space: a read of it fills
  the caller's buffer with ``0xFF`` and succeeds, like a raw block device.
- No ``erase_blocks`` callback is offered: ``ra8_fs`` only uses an erase that
  guarantees a ZERO read-back, and NOR erases to ones, so the formatter falls
  back to writing zeros.
- The adapter is stateless apart from one static sector bounce buffer used to
  satisfy LevelX's ``ULONG``-aligned buffer contract; ``ra8_fs`` serialises
  every backend call (``ra8_fs_set_lock()``), so the buffer is never shared
  between two calls.

## Consumers

- ``threadx_fs_demo`` and ``threadx_fs_levelx_demo`` (HIL apps under
  ``examples/ek_ra8d2/hw_validated/hil/``) call ``lx_fs_backend_bind`` first so
  ``ra8_fs`` has a block device to format and mount.
- ``ra8_cache_store`` mounts over LevelX's public API: the ``lx_nor_flash_*``
  names are macros in ``lx_api.h``, so its Zig externs bind the ``_lx_*``
  symbols directly (see ``libs/ra8_cache_store/src/mount.zig``).
- ``test_lx_fs_backend`` (host): runs the whole demos' storage stack minus only
  the xSPI silicon -- real standalone LevelX over ``lx_nor_fake_ram`` plus the
  shim under test, with ``ra8_fs`` objects linked in. Registered by hand in
  ``tests/cmake/tests_storage.cmake`` because the vendored LevelX core and the
  fake driver sit outside the ``ra8_add_test()`` auto-glob.
- ``ra8_ftl`` is the alternative first-party wear-levelling layer; the
  trade-offs between the two layers are laid out in ``libs/ra8_ftl/README.md``.

## FileX retirement

The vendored FileX integration this port once fed is retired; the filesystem
path today is the one above (LevelX -> ``lx_fs_backend`` -> ``ra8_fs``). FileX
references remain only in historical demo READMEs and doc commentary, not in
the storage stack itself.
