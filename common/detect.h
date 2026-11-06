/*
 * OpenBOR multi-engine libretro core - engine version detection.
 */
#ifndef OB_DETECT_H
#define OB_DETECT_H

typedef struct
{
    int version;        /* chosen engine id, -1 if unknown */
    char reason[256];   /* human readable explanation (for the log) */
    int webm_files;     /* number of .webm videos in the pak */
    int sab_calls;      /* script calls of SAB extensions (sab_*): needs 8025_webm */
} ob_detect_result;

/* Inspects the .pak and picks the most suitable engine among `ids`
 * (sorted oldest first). */
void ob_detect_version(const char *pak_path, const int *ids, int count, ob_detect_result *out);

#endif
