/* WARNING! All changes made to this file will be lost! */

#ifndef _LILV_CONFIG_H_WAF
#define _LILV_CONFIG_H_WAF

#define HAVE_LV2CORE 1
#define HAVE_SORD 1
#undef  HAVE_WORDEXP
#define HAVE_LV2_LV2PLUG_IN_NS_LV2CORE_LV2_H 1
#define LILV_VERSION "0.5.0"
#define LILV_PATH_SEP ":"
#define LILV_DIR_SEP "/"
#define LILV_DEFAULT_LV2_PATH "~/Library/Audio/Plug-Ins/LV2:~/.lv2:/usr/local/lib/lv2:/usr/lib/lv2:/Library/Audio/Plug-Ins/LV2"

#endif /* _LILV_CONFIG_H_WAF */
