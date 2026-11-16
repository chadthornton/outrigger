/* faust_glue.h: the two interface structs faust -lang c refers to (MetaGlue, UIGlue), cut down to the members
   faust_voice.h uses. Written for this bench (Faust's own CInterface.h is LGPL-2.1; not copied). The Vibe never calls
   metadatavoice or buildUserInterfacevoice: it sets the voice's fields directly. */
#pragma once
typedef float FAUSTFLOAT_T;
typedef struct {
  void *metaInterface;
  void (*declare)(void *m, const char *key, const char *value);
} MetaGlue;
typedef struct {
  void *uiInterface;
  void (*openVerticalBox)(void *ui, const char *label);
  void (*closeBox)(void *ui);
  void (*addButton)(void *ui, const char *label, FAUSTFLOAT_T *zone);
  void (*addHorizontalSlider)(void *ui, const char *label, FAUSTFLOAT_T *zone, FAUSTFLOAT_T init, FAUSTFLOAT_T min,
                              FAUSTFLOAT_T max, FAUSTFLOAT_T step);
} UIGlue;
