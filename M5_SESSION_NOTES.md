# SDL30 Collector — M5 Export Session Notes

## Build state
- Compiles successfully (verified April 23, 2026)
- NOT flashed or tested yet
- No real M5 output verified

## Known unresolved issues
- M5 Z record placement for BFFB — code SHOULD write Z after BS2 with mean RL
  but this was never tested with real data
- Notes backend exists (storage_save_note/get_note/load_notes)
  but note TO records are NOT yet inserted into M5 export output
- Web UI note ● indicator + editNote() added but not tested

## Next steps for new Claude
1. Flash firmware: idf.py flash
2. Create a test job, import 18001_section1.csv as test data
3. Tap M5/Zeiss button, download the .m5 file
4. Compare output line-by-line against the real DiNi examples:
   - M5_example_5_sections.txt
   - us41.dat
5. Verify info block columns: PNo(8)+Code(5)+6sp+Sno(4)+Zno(4)=27
6. Verify BFFB Z comes after BS2 not after FS2
7. Re-add note TO records to M5 export (cleanly, without the sed mess)

## Files in this session's git diff
- main/storage/m5_export.c (new)
- main/storage/m5_export.h (new)  
- main/storage/storage.c (+notes functions appended)
- main/storage/storage.h (+notes declarations)
- main/sdl30_types.h (+setup_no fields)
- main/web/web_server.c (+M5 download endpoint +note endpoints)
- main/web/web_ui.h (+note indicator +editNote JS)
- main/CMakeLists.txt (+m5_export.c source)

## Test data files (in ~/Downloads)
- 18001_section1.csv — 39 records BF, 19 setups
- us41.csv — 10 records BF+IS, 4 setups
