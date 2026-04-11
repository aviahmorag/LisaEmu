/*
 * Shared source-enumeration exclusion rules.
 * See toolchain_fileset.h.
 */

#include "toolchain_fileset.h"

#include <string.h>
#include <strings.h>

bool tc_should_skip_dir(const char *name) {
    if (strcasecmp(name, "BUILD") == 0) return true;
    if (strcasecmp(name, "OS exec files") == 0) return true;
    if (strcasestr(name, "Linkmaps") != NULL) return true;
    if (strcasestr(name, "exec files") != NULL) return true;
    if (strcasecmp(name, "DICT") == 0) return true;
    if (strcasecmp(name, "Fonts") == 0) return true;
    if (strcasecmp(name, "FONTS") == 0) return true;
    if (strcasecmp(name, "APPS") == 0) return true;
    if (strcasecmp(name, "Lisa_Toolkit") == 0) return true;
    if (strcasecmp(name, "APIN") == 0) return true;       /* install scripts */
    if (strcasecmp(name, "GUIDE_APIM") == 0) return true; /* guide/tutorial app */
    if (strcasecmp(name, "TKIN") == 0) return true;       /* toolkit install */
    /* Sample app directories — build scripts, not compilable units */
    if (strcasecmp(name, "TK3") == 0) return true;
    if (strcasecmp(name, "TK4") == 0) return true;
    if (strcasecmp(name, "TK5") == 0) return true;
    return false;
}

bool tc_should_skip_file(const char *name) {
    /* Match on leaf filename only (not full path). */

    /* --- Build / install / link / doc scripts -------------------------- */
    if (strncasecmp(name, "BUILD-", 6) == 0) return true;
    if (strcasestr(name, "-BUILDLLD") != NULL) return true;  /* lib build scripts */
    if (strcasestr(name, "LINK.TEXT") != NULL) return true;  /* link command files */
    if (strcasestr(name, "linkmap") != NULL) return true;
    if (strcasestr(name, "COMP.TEXT") != NULL) return true;
    if (strcasestr(name, "INSTALL.TEXT") != NULL) return true;
    if (strcasestr(name, "DOC.TEXT") != NULL) return true;
    if (strcasestr(name, "PMdoc") != NULL) return true;
    if (strcasestr(name, "APPENDIX") != NULL) return true;
    if (strcasestr(name, "SUMMARY") != NULL) return true;
    if (strcasestr(name, "INSTRUCT") != NULL) return true;
    if (strcasestr(name, "RELEASE") != NULL) return true;
    if (strcasestr(name, "relmemo") != NULL) return true;
    /* libhw-REL / libhw-DOC etc. — documentation/release memos.
     * DOC.TEXT / APPENDIX above already cover -DOC and -APPENDIX cases;
     * -REL is more ambiguous, restrict to the libhw variant that is
     * definitely a release memo. */
    if (strncasecmp(name, "libhw-REL", 9) == 0) return true;
    if (strncasecmp(name, "LibHW-REL", 9) == 0) return true;
    if (strncasecmp(name, "LIBHW-REL", 9) == 0) return true;

    /* --- Plain-text resources / alerts / tables / lists ---------------- */
    if (strcasestr(name, "ALERT") != NULL) return true;
    if (strcasestr(name, "-TABLES.TEXT") != NULL) return true;
    if (strcasestr(name, "-LIST.TEXT") != NULL) return true;
    if (strcasestr(name, "-SIZES.TEXT") != NULL) return true;
    if (strcasestr(name, "-EXEC.TEXT") != NULL) return true;

    /* --- LIBHW include fragments (assembled via DRIVERS.TEXT master) --- */
    if (strcasestr(name, "libhw-CURSOR") != NULL) return true;
    if (strcasestr(name, "libhw-KEYBD") != NULL) return true;
    if (strcasestr(name, "libhw-LEGENDS") != NULL) return true;
    if (strcasestr(name, "libhw-MACHINE") != NULL) return true;
    if (strcasestr(name, "libhw-MOUSE") != NULL) return true;
    if (strcasestr(name, "libhw-SPRKEYBD") != NULL) return true;
    if (strcasestr(name, "libhw-TIMERS") != NULL) return true;

    /* --- LIBQD include fragments (assembled via DRAWLINE master) ------- */
    if (strcasestr(name, "FASTLINE") != NULL) return true;
    if (strcasestr(name, "LINE2") != NULL) return true;
    if (strcasestr(name, "GRAFTYPES") != NULL) return true;
    /* STRETCH stays — it's standalone assembly, not an include fragment. */

    /* --- App-level data, menus, dialogs, phrase/resource files --------- */
    if (strcasestr(name, "T5LM") != NULL) return true;
    if (strcasestr(name, "t5dbc") != NULL) return true;
    if (strcasestr(name, "t8dialogs") != NULL) return true;
    if (strcasestr(name, "t10menus") != NULL) return true;
    if (strcasestr(name, "T10DBOX") != NULL) return true;
    if (strcasestr(name, "PABC") != NULL) return true;
    if (strcasestr(name, "PASGEN") != NULL) return true;
    if (strcasestr(name, "phquickport") != NULL) return true;
    if (strcasestr(name, "INITFPFILE") != NULL) return true;
    if (strcasestr(name, "qpsample") != NULL) return true;
    if (strcasestr(name, "qpmake") != NULL) return true;
    if (strcasestr(name, "make_qp") != NULL) return true;
    if (strcasestr(name, "link_qp") != NULL) return true;
    if (strcasestr(name, "lnewFPLIB") != NULL) return true;
    if (strcasestr(name, "buildpref") != NULL) return true;
    if (strcasestr(name, "MAKEHEUR") != NULL) return true;
    if (strcasestr(name, "LETTERCODES") != NULL) return true;
    if (strcasestr(name, "KEYWORDS") != NULL) return true;
    if (strcasestr(name, "FKEYWORDS") != NULL) return true;
    if (strcasestr(name, "CNBUILD") != NULL) return true;
    if (strcasestr(name, "CIBUILD") != NULL) return true;
    if (strcasestr(name, "BUILDPR") != NULL) return true;
    if (strcasestr(name, "DWBTN") != NULL) return true;
    if (strcasestr(name, "ciBTN") != NULL) return true;
    if (strcasestr(name, "PARBTN") != NULL) return true;
    if (strcasestr(name, "CNBTN") != NULL) return true;
    if (strcasestr(name, "PASLIBDOC") != NULL) return true;
    if (strcasestr(name, "PASLIBCDOC") != NULL) return true;

    /* --- Utilities and sample programs that aren't OS components ------- */
    if (strcasestr(name, "KEYBOARD.TEXT") != NULL) return true;
    if (strcasestr(name, "STUNTS") != NULL) return true;
    if (strcasestr(name, "DRVRMAIN") != NULL) return true;
    if (strcasestr(name, "PEPSITESTS") != NULL) return true;
    if (strcasestr(name, "MAINBAUD") != NULL) return true;
    if (strcasestr(name, "copymaster") != NULL) return true;
    if (strcasestr(name, "bless") != NULL) return true;
    if (strcasestr(name, "ALERTGEN") != NULL) return true;
    if (strcasestr(name, "REALPASLIB") != NULL) return true;
    if (strcasestr(name, "FPPASLIB") != NULL) return true;
    if (strcasestr(name, "GDATALIST") != NULL) return true;
    if (strcasestr(name, "cdchar") != NULL) return true;
    if (strcasestr(name, "nwshell") != NULL) return true;
    if (strcasestr(name, "cdCONFIG") != NULL) return true;
    if (strcasestr(name, "TKALERT") != NULL) return true;

    return false;
}
