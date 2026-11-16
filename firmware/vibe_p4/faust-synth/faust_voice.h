/* ------------------------------------------------------------
license: "MIT"
name: "faust-voice"
Code generated with Faust 2.88.0 (https://faust.grame.fr)
Compilation options: -lang c -fpga-mem-th 4 -ct 1 -cn voice -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0
------------------------------------------------------------ */

#ifndef  __voice_H__
#define  __voice_H__

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 


#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef FAUST_INT_WRAP
#define FAUST_INT_WRAP
static inline int faust_wrap_add(int a, int b) { return (int)((unsigned int)a + (unsigned int)b); }
static inline int faust_wrap_sub(int a, int b) { return (int)((unsigned int)a - (unsigned int)b); }
static inline int faust_wrap_mul(int a, int b) { return (int)((unsigned int)a * (unsigned int)b); }
#endif


static float voice_faustpower2_f(float value) {
	return value * value;
}

#ifndef FAUSTCLASS 
#define FAUSTCLASS voice
#endif

#ifdef __APPLE__ 
#define exp10f __exp10f
#define exp10 __exp10
#endif
#ifndef FAUSTMAXI
#define FAUSTMAXI
static inline int faustmaxi(int a, int b) { return (a > b) ? a : b; }
static inline int faustmini(int a, int b) { return (a < b) ? a : b; }
#endif

typedef struct {
	FAUSTFLOAT fHslider0;
	FAUSTFLOAT fHslider1;
	int fSampleRate;
	float fConst0;
	float fConst1;
	FAUSTFLOAT fHslider2;
	float fConst2;
	FAUSTFLOAT fHslider3;
	float fRec1[2];
	float fRec0[3];
	float fConst3;
	float fConst4;
	FAUSTFLOAT fButton0;
	float fVec0[2];
	float fRec2[2];
	float fConst5;
	float fConst6;
	int iRec3[2];
} voice;

voice* newvoice() { 
	voice* dsp = (voice*)calloc(1, sizeof(voice));
	return dsp;
}

void deletevoice(voice* dsp) { 
	free(dsp);
}

void metadatavoice(MetaGlue* m) { 
	m->declare(m->metaInterface, "compile_options", "-lang c -fpga-mem-th 4 -ct 1 -cn voice -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0");
	m->declare(m->metaInterface, "envelopes.lib/adsr:author", "Yann Orlarey and Andrey Bundin");
	m->declare(m->metaInterface, "envelopes.lib/author", "GRAME");
	m->declare(m->metaInterface, "envelopes.lib/copyright", "GRAME");
	m->declare(m->metaInterface, "envelopes.lib/license", "LicenseRef-LGPL-2.1-or-later-with-Faust-exception");
	m->declare(m->metaInterface, "envelopes.lib/name", "Faust Envelope Library");
	m->declare(m->metaInterface, "envelopes.lib/version", "1.3.0");
	m->declare(m->metaInterface, "filename", "voice.dsp");
	m->declare(m->metaInterface, "filters.lib/fir:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "filters.lib/fir:copyright", "Copyright (C) 2003-2019 by Julius O. Smith III <jos@ccrma.stanford.edu>");
	m->declare(m->metaInterface, "filters.lib/fir:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "filters.lib/iir:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "filters.lib/iir:copyright", "Copyright (C) 2003-2019 by Julius O. Smith III <jos@ccrma.stanford.edu>");
	m->declare(m->metaInterface, "filters.lib/iir:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "filters.lib/name", "Faust Filters Library");
	m->declare(m->metaInterface, "filters.lib/resonlp:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "filters.lib/resonlp:copyright", "Copyright (C) 2003-2019 by Julius O. Smith III <jos@ccrma.stanford.edu>");
	m->declare(m->metaInterface, "filters.lib/resonlp:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "filters.lib/tf2:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "filters.lib/tf2:copyright", "Copyright (C) 2003-2019 by Julius O. Smith III <jos@ccrma.stanford.edu>");
	m->declare(m->metaInterface, "filters.lib/tf2:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "filters.lib/tf2s:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "filters.lib/tf2s:copyright", "Copyright (C) 2003-2019 by Julius O. Smith III <jos@ccrma.stanford.edu>");
	m->declare(m->metaInterface, "filters.lib/tf2s:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "filters.lib/version", "1.9.0");
	m->declare(m->metaInterface, "license", "MIT");
	m->declare(m->metaInterface, "maths.lib/author", "GRAME");
	m->declare(m->metaInterface, "maths.lib/copyright", "GRAME");
	m->declare(m->metaInterface, "maths.lib/license", "LicenseRef-LGPL-2.1-or-later-with-Faust-exception");
	m->declare(m->metaInterface, "maths.lib/name", "Faust Math Library");
	m->declare(m->metaInterface, "maths.lib/version", "2.9.0");
	m->declare(m->metaInterface, "name", "faust-voice");
	m->declare(m->metaInterface, "oscillators.lib/name", "Faust Oscillator Library");
	m->declare(m->metaInterface, "oscillators.lib/saw2ptr:author", "Julius O. Smith III");
	m->declare(m->metaInterface, "oscillators.lib/saw2ptr:license", "LicenseRef-STK-4.3");
	m->declare(m->metaInterface, "oscillators.lib/version", "1.8.0");
	m->declare(m->metaInterface, "platform.lib/name", "Generic Platform Library");
	m->declare(m->metaInterface, "platform.lib/version", "1.3.0");
}

int getSampleRatevoice(voice* RESTRICT dsp) {
	return dsp->fSampleRate;
}

int getNumInputsvoice(voice* RESTRICT dsp) {
	return 0;
}
int getNumOutputsvoice(voice* RESTRICT dsp) {
	return 1;
}

void classInitvoice(int sample_rate) {
}

void instanceResetUserInterfacevoice(voice* dsp) {
	dsp->fHslider0 = (FAUSTFLOAT)(0.5f);
	dsp->fHslider1 = (FAUSTFLOAT)(1.2f);
	dsp->fHslider2 = (FAUSTFLOAT)(1.8e+03f);
	dsp->fHslider3 = (FAUSTFLOAT)(4.4e+02f);
	dsp->fButton0 = (FAUSTFLOAT)(0.0f);
}

void instanceClearvoice(voice* dsp) {
	/* C99 loop */
	{
		int l0;
		for (l0 = 0; l0 < 2; l0 = faust_wrap_add(l0, 1)) {
			dsp->fRec1[l0] = 0.0f;
		}
	}
	/* C99 loop */
	{
		int l1;
		for (l1 = 0; l1 < 3; l1 = faust_wrap_add(l1, 1)) {
			dsp->fRec0[l1] = 0.0f;
		}
	}
	/* C99 loop */
	{
		int l2;
		for (l2 = 0; l2 < 2; l2 = faust_wrap_add(l2, 1)) {
			dsp->fVec0[l2] = 0.0f;
		}
	}
	/* C99 loop */
	{
		int l3;
		for (l3 = 0; l3 < 2; l3 = faust_wrap_add(l3, 1)) {
			dsp->fRec2[l3] = 0.0f;
		}
	}
	/* C99 loop */
	{
		int l4;
		for (l4 = 0; l4 < 2; l4 = faust_wrap_add(l4, 1)) {
			dsp->iRec3[l4] = 0;
		}
	}
}

void instanceConstantsvoice(voice* dsp, int sample_rate) {
	dsp->fSampleRate = sample_rate;
	dsp->fConst0 = fminf(1.92e+05f, fmaxf(1.0f, (float)(dsp->fSampleRate)));
	dsp->fConst1 = 3.1415927f / dsp->fConst0;
	dsp->fConst2 = 1.0f / dsp->fConst0;
	dsp->fConst3 = fmaxf(1.0f, 0.005f * dsp->fConst0);
	dsp->fConst4 = 1.0f / dsp->fConst3;
	dsp->fConst5 = 0.3f / fmaxf(1.0f, 0.3f * dsp->fConst0);
	dsp->fConst6 = 1.0f / fmaxf(1.0f, 0.4f * dsp->fConst0);
}
	
void instanceInitvoice(voice* dsp, int sample_rate) {
	instanceConstantsvoice(dsp, sample_rate);
	instanceResetUserInterfacevoice(dsp);
	instanceClearvoice(dsp);
}

void initvoice(voice* dsp, int sample_rate) {
	classInitvoice(sample_rate);
	instanceInitvoice(dsp, sample_rate);
}

void buildUserInterfacevoice(voice* dsp, UIGlue* ui_interface) {
	ui_interface->openVerticalBox(ui_interface->uiInterface, "faust-voice");
	ui_interface->addHorizontalSlider(ui_interface->uiInterface, "cutoff", &dsp->fHslider2, (FAUSTFLOAT)1.8e+03f, (FAUSTFLOAT)4e+01f, (FAUSTFLOAT)1.2e+04f, (FAUSTFLOAT)1.0f);
	ui_interface->addHorizontalSlider(ui_interface->uiInterface, "freq", &dsp->fHslider3, (FAUSTFLOAT)4.4e+02f, (FAUSTFLOAT)2e+01f, (FAUSTFLOAT)2e+04f, (FAUSTFLOAT)0.01f);
	ui_interface->addHorizontalSlider(ui_interface->uiInterface, "gain", &dsp->fHslider0, (FAUSTFLOAT)0.5f, (FAUSTFLOAT)0.0f, (FAUSTFLOAT)1.0f, (FAUSTFLOAT)0.001f);
	ui_interface->addButton(ui_interface->uiInterface, "gate", &dsp->fButton0);
	ui_interface->addHorizontalSlider(ui_interface->uiInterface, "q", &dsp->fHslider1, (FAUSTFLOAT)1.2f, (FAUSTFLOAT)0.5f, (FAUSTFLOAT)8.0f, (FAUSTFLOAT)0.01f);
	ui_interface->closeBox(ui_interface->uiInterface);
}

void computevoice(voice* dsp, int count, FAUSTFLOAT** RESTRICT inputs, FAUSTFLOAT** RESTRICT outputs) {
	FAUSTFLOAT* output0 = outputs[0];
	float fSlow0 = 1.0f / (float)(dsp->fHslider1);
	float fSlow1 = tanf(dsp->fConst1 * (float)(dsp->fHslider2));
	float fSlow2 = 1.0f / fSlow1;
	float fSlow3 = (fSlow0 + fSlow2) / fSlow1 + 1.0f;
	float fSlow4 = (float)(dsp->fHslider0) / fSlow3;
	float fSlow5 = 1.0f / fSlow3;
	float fSlow6 = (fSlow2 - fSlow0) / fSlow1 + 1.0f;
	float fSlow7 = 2.0f * (1.0f - 1.0f / voice_faustpower2_f(fSlow1));
	float fSlow8 = fmaxf(1.1920929e-07f, fabsf((float)(dsp->fHslider3)));
	float fSlow9 = dsp->fConst2 * fSlow8;
	float fSlow10 = fSlow9 + -1.0f;
	float fSlow11 = 1.0f - dsp->fConst0 / fSlow8;
	float fSlow12 = (float)(dsp->fButton0);
	int iSlow13 = fSlow12 == 0.0f;
	/* C99 loop */
	{
		int i0;
		for (i0 = 0; i0 < count; i0 = faust_wrap_add(i0, 1)) {
			float fTemp0 = fSlow10 + dsp->fRec1[1];
			int iTemp1 = fTemp0 < 0.0f;
			float fTemp2 = fSlow9 + dsp->fRec1[1];
			dsp->fRec1[0] = ((iTemp1) ? fTemp2 : fTemp0);
			dsp->fRec0[0] = -1.0f - fSlow5 * (fSlow6 * dsp->fRec0[2] + fSlow7 * dsp->fRec0[1]) + 2.0f * ((iTemp1) ? fTemp2 : fSlow9 + dsp->fRec1[1] + fSlow11 * fTemp0);
			dsp->fVec0[0] = fSlow12;
			dsp->fRec2[0] = fSlow12 + dsp->fRec2[1] * (float)(dsp->fVec0[1] >= fSlow12);
			dsp->iRec3[0] = faust_wrap_mul(iSlow13, faust_wrap_add(1, dsp->iRec3[1]));
			output0[i0] = (FAUSTFLOAT)(fSlow4 * (dsp->fRec0[2] + dsp->fRec0[0] + 2.0f * dsp->fRec0[1]) * fmaxf(0.0f, fminf(dsp->fConst4 * dsp->fRec2[0], fmaxf(1.0f + dsp->fConst5 * (dsp->fConst3 - dsp->fRec2[0]), 0.7f)) * (1.0f - dsp->fConst6 * (float)(dsp->iRec3[0]))));
			dsp->fRec1[1] = dsp->fRec1[0];
			dsp->fRec0[2] = dsp->fRec0[1];
			dsp->fRec0[1] = dsp->fRec0[0];
			dsp->fVec0[1] = dsp->fVec0[0];
			dsp->fRec2[1] = dsp->fRec2[0];
			dsp->iRec3[1] = dsp->iRec3[0];
		}
	}
}

#ifdef __cplusplus
}
#endif

#endif
