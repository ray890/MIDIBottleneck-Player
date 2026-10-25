/* The raw MIDI stream entry points of Sonivox (EAS_OpenMIDIStream, EAS_WriteMIDIStream, EAS_CloseMIDIStream).
 *
 * They are declared in the public header (host_src/eas.h) but their source is not part of the open-source release in
 * Android's external/sonivox (only the prebuilt libraries of the original vendor contain them; Android itself only plays
 * MIDI files through this library). This file is MIDIBottleneck Player's own implementation of those three functions
 * on top of Sonivox's internal MIDI parser, written for this player (it is the player's own code, not part of the
 * vendored Sonivox files), following how Sonivox's own SMF reader feeds the synth
 * (lib_src/eas_smf.c: VMInitMIDI, EAS_ParseMIDIStream, VMMIDIShutdown). It is compiled with the Sonivox library
 * (v2/android/native/CMakeLists.txt) because it needs Sonivox's internal headers.
 *
 * One stream per EAS instance is what the player uses; the stream does not occupy one of Sonivox's four file/parser
 * slots, so Sonivox's own bookkeeping (EAS_Render, EAS_Shutdown) never sees it. Not thread-safe, like the rest of Sonivox.
 */
#include "eas.h"
#include "eas_data.h"
#include "eas_host.h"
#include "eas_miditypes.h"
#include "eas_midi.h"
#include "eas_mdls.h"
#include "eas_report.h"
#include "eas_vm_protos.h"
#include <string.h>

typedef struct
{
    S_SYNTH         *pSynth;
    S_MIDI_STREAM   midiStream;
} MIDIBN_MIDI_STREAM;

EAS_PUBLIC EAS_RESULT EAS_OpenMIDIStream (EAS_DATA_HANDLE pEASData, EAS_HANDLE *pStreamHandle, EAS_HANDLE streamHandle)
{
    MIDIBN_MIDI_STREAM *pStream;
    EAS_RESULT result;

    (void) streamHandle;   /* "open stream or NULL for new synthesizer instance": sharing a synthesizer is not supported */
    if (pEASData == NULL || pStreamHandle == NULL)
        return EAS_ERROR_PARAMETER_RANGE;
    *pStreamHandle = NULL;

    pStream = (MIDIBN_MIDI_STREAM *) EAS_HWMalloc(pEASData->hwInstData, (EAS_I32) sizeof(MIDIBN_MIDI_STREAM));
    if (pStream == NULL)
        return EAS_ERROR_MALLOC_FAILED;
    EAS_HWMemSet(pStream, 0, (EAS_I32) sizeof(MIDIBN_MIDI_STREAM));

    if ((result = VMInitMIDI(pEASData, &pStream->pSynth)) != EAS_SUCCESS)
    {
        EAS_HWFree(pEASData->hwInstData, pStream);
        return result;
    }
    EAS_InitMIDIStream(&pStream->midiStream);
    *pStreamHandle = (EAS_HANDLE) pStream;
    return EAS_SUCCESS;
}

EAS_PUBLIC EAS_RESULT EAS_WriteMIDIStream (EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle, EAS_U8 *pBuffer, EAS_I32 count)
{
    MIDIBN_MIDI_STREAM *pStream = (MIDIBN_MIDI_STREAM *) streamHandle;
    EAS_RESULT result = EAS_SUCCESS;
    EAS_I32 i;

    if (pEASData == NULL || pStream == NULL || pStream->pSynth == NULL || (pBuffer == NULL && count > 0))
        return EAS_ERROR_PARAMETER_RANGE;

    for (i = 0; i < count; i++)
    {
        EAS_RESULT one = EAS_ParseMIDIStream(pEASData, pStream->pSynth, &pStream->midiStream, pBuffer[i], eParserModePlay);
        if (one != EAS_SUCCESS)
            result = one;   /* keep feeding: one bad message must not swallow the rest of the burst */
    }
    return result;
}

EAS_PUBLIC EAS_RESULT EAS_CloseMIDIStream (EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle)
{
    MIDIBN_MIDI_STREAM *pStream = (MIDIBN_MIDI_STREAM *) streamHandle;

    if (pEASData == NULL || pStream == NULL)
        return EAS_ERROR_PARAMETER_RANGE;
    if (pStream->pSynth != NULL)
        VMMIDIShutdown(pEASData, pStream->pSynth);
    EAS_HWFree(pEASData->hwInstData, pStream);
    return EAS_SUCCESS;
}

/* ---- Settings that Sonivox's public header does not reach for a raw MIDI stream (render thread only, like everything here) ---- */

/* Voice limit of the stream (1 up to the build's maximum, MAX_SYNTH_VOICES); voices above a new, lower limit are stolen at once. */
EAS_RESULT MIDIBN_SetStreamPolyphony (EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle, EAS_I32 voices)
{
    MIDIBN_MIDI_STREAM *pStream = (MIDIBN_MIDI_STREAM *) streamHandle;
    EAS_RESULT result;

    if (pEASData == NULL || pStream == NULL || pStream->pSynth == NULL)
        return EAS_ERROR_PARAMETER_RANGE;
    if (voices < 1 || voices > MAX_SYNTH_VOICES)
        return EAS_ERROR_PARAMETER_RANGE;
    result = VMSetPolyphony(pEASData->pVoiceMgr, pStream->pSynth, voices);
    if (result != EAS_SUCCESS)
        return result;

    /* VMSetPolyphony is meant to also mute the voices above a new, lower limit, but it looks for them by "nextChannel", which
     * Sonivox clears once a voice has started, so it never finds one that is already sounding. Do that here: mute the oldest
     * sounding voices of this stream until the limit is met (the same age-first choice Sonivox makes when it steals a voice). */
    for (;;)
    {
        S_VOICE_MGR *pVoiceMgr = pEASData->pVoiceMgr;
        EAS_INT i, active = 0, oldest = -1;
        EAS_I32 oldestAge = -1;

        for (i = 0; i < MAX_SYNTH_VOICES; i++)
        {
            S_SYNTH_VOICE *pVoice = &pVoiceMgr->voices[i];

            if (pVoice->voiceState == eVoiceStateFree || pVoice->voiceState == eVoiceStateMuting)
                continue;
            if (GET_VSYNTH(pVoice->channel) != pStream->pSynth->vSynthNum)
                continue;
            active++;
            if ((EAS_I32) pVoice->age > oldestAge)
            {
                oldestAge = (EAS_I32) pVoice->age;
                oldest = i;
            }
        }
        if (active <= voices || oldest < 0)
            break;
        VMMuteVoice(pVoiceMgr, oldest);
    }
    return EAS_SUCCESS;
}

/* Voices sounding right now (including the ones in their release). */
EAS_I32 MIDIBN_StreamActiveVoices (EAS_HANDLE streamHandle)
{
    MIDIBN_MIDI_STREAM *pStream = (MIDIBN_MIDI_STREAM *) streamHandle;

    if (pStream == NULL || pStream->pSynth == NULL)
        return 0;
    return (EAS_I32) pStream->pSynth->numActiveVoices;
}

/* The voice maximum this library was built with. */
EAS_I32 MIDIBN_MaxVoices (void)
{
    return (EAS_I32) MAX_SYNTH_VOICES;
}

/* ---- DLS sound banks --------------------------------------------------------------------------------------------------
 * The open-source Sonivox has the DLS parser (DLSParser, eas_mdls.c) but not the public calls that load a collection
 * (EAS_LoadDLSCollection). These functions are the player's own version for a raw MIDI stream.
 *
 *  - MIDIBN_ParseDLS reads a whole bank into memory through two read callbacks. It touches nothing but its own temporary
 *    host data (EAS_HWInit), so it may run on any thread while another thread renders. The result has one reference.
 *  - MIDIBN_SetStreamDLS hands a parsed bank to the stream (NULL = the built-in sounds again). Render thread only, and
 *    reference counts are not atomic in Sonivox: the C++ side calls Add/Release/SetStreamDLS under one mutex.
 */

static EAS_U32 MIDIBN_ReadLE32 (const EAS_U8 *p)
{
    return (EAS_U32) p[0] | ((EAS_U32) p[1] << 8) | ((EAS_U32) p[2] << 16) | ((EAS_U32) p[3] << 24);
}

/* Walks RIFF DLS > LIST lins > LIST ins > insh and moves each instrument to where a MIDI player looks for it. A channel
 * starts in bank 0x79 (melodic) or 0x78 (rhythm) and Sonivox finds a DLS sound by the exact bank number, so a General MIDI
 * bank (bank 0, drums marked by bit 31 of the bank number, which Sonivox's own parser drops) would never be used. */
static void MIDIBN_RemapPrograms (S_DLS *pDLS, int (*readAt)(void *, void *, int, int), int (*sizeOf)(void *), void *handle)
{
    EAS_U8 head[12], hdr[12];
    EAS_I32 fileSize = sizeOf(handle), pos, linsEnd;
    int index = 0;

    if (fileSize < 12 || readAt(handle, head, 0, 12) != 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "DLS ", 4) != 0)
        return;
    pos = 12;
    /* find LIST lins */
    for (;;)
    {
        EAS_U32 size;
        if (pos + 12 > fileSize || readAt(handle, hdr, pos, 12) != 12) return;
        size = MIDIBN_ReadLE32(hdr + 4);
        if (size > 0x7fffffffu - 16u) return;
        if (memcmp(hdr, "LIST", 4) == 0 && memcmp(hdr + 8, "lins", 4) == 0)
        {
            linsEnd = pos + 8 + (EAS_I32) size;
            pos += 12;
            break;
        }
        pos += 8 + (EAS_I32) size + (EAS_I32) (size & 1);
    }
    if (linsEnd > fileSize) linsEnd = fileSize;
    while (pos + 12 <= linsEnd && index < (int) pDLS->numDLSPrograms)
    {
        EAS_U32 size;
        if (readAt(handle, hdr, pos, 12) != 12) return;
        size = MIDIBN_ReadLE32(hdr + 4);
        if (size > 0x7fffffffu - 16u) return;
        if (memcmp(hdr, "LIST", 4) == 0 && memcmp(hdr + 8, "ins ", 4) == 0)
        {
            EAS_I32 q = pos + 12, insEnd = pos + 8 + (EAS_I32) size;
            EAS_U8 ih[8], insh[12];
            while (q + 8 <= insEnd && q + 8 <= fileSize)
            {
                EAS_U32 csize;
                if (readAt(handle, ih, q, 8) != 8) return;
                csize = MIDIBN_ReadLE32(ih + 4);
                if (csize > 0x7fffffffu - 16u) return;
                if (memcmp(ih, "insh", 4) == 0 && csize >= 12)
                {
                    EAS_U32 bank, program, msb, lsb;
                    S_PROGRAM *pProgram = &pDLS->pDLSPrograms[index];
                    if (readAt(handle, insh, q + 8, 12) != 12) return;
                    bank = MIDIBN_ReadLE32(insh + 4);
                    program = MIDIBN_ReadLE32(insh + 8) & 0x7f;
                    msb = (bank >> 8) & 0x7f;
                    lsb = bank & 0x7f;
                    if (bank & 0x80000000u)
                        pProgram->locale = ((EAS_U32) 0x78 << 16) | program;                       /* drum kit: the rhythm bank */
                    else if (msb == 0)
                        pProgram->locale = ((EAS_U32) 0x79 << 16) | (lsb << 8) | program;          /* melodic bank 0 = the default bank */
                    break;
                }
                q += 8 + (EAS_I32) csize + (EAS_I32) (csize & 1);
            }
            index++;
        }
        pos += 8 + (EAS_I32) size + (EAS_I32) (size & 1);
    }
}

/* Reads a DLS bank. readAt(handle, buf, offset, size) returns the bytes read; sizeOf(handle) is the size of the bank in bytes.
 * Returns the bank (one reference) or NULL with *pResult set to the Sonivox error number. */
void *MIDIBN_ParseDLS (int (*readAt)(void *, void *, int, int), int (*sizeOf)(void *), void *handle, EAS_RESULT *pResult)
{
    EAS_FILE locator;
    EAS_HW_DATA_HANDLE hw = NULL;
    EAS_FILE_HANDLE file = NULL;
    S_DLS *pDLS = NULL;
    EAS_RESULT result;

    locator.handle = handle;
    locator.readAt = readAt;
    locator.size = sizeOf;
    if ((result = EAS_HWInit(&hw)) != EAS_SUCCESS) { *pResult = result; return NULL; }
    result = EAS_HWOpenFile(hw, &locator, &file, EAS_FILE_READ);
    if (result == EAS_SUCCESS)
    {
        result = DLSParser(hw, file, 0, &pDLS);
        EAS_HWCloseFile(hw, file);
    }
    EAS_HWShutdown(hw);
    if (result != EAS_SUCCESS || pDLS == NULL)
    {
        if (pDLS != NULL) DLSCleanup(NULL, pDLS);
        *pResult = (result != EAS_SUCCESS) ? result : EAS_FAILURE;
        return NULL;
    }
    MIDIBN_RemapPrograms(pDLS, readAt, sizeOf, handle);
    *pResult = EAS_SUCCESS;
    return pDLS;
}

void MIDIBN_AddRefDLS (void *dls) { DLSAddRef((S_DLS *) dls); }

void MIDIBN_ReleaseDLS (void *dls) { if (dls != NULL) DLSCleanup(NULL, (S_DLS *) dls); }

/* Gives the stream a bank (the stream keeps one reference: the caller has already added it) or, with NULL, the built-in
 * sounds. The sounding notes are cut (they point into the old bank); the channels keep their programs, volumes and so on. */
EAS_RESULT MIDIBN_SetStreamDLS (EAS_DATA_HANDLE pEASData, EAS_HANDLE streamHandle, void *dls)
{
    MIDIBN_MIDI_STREAM *pStream = (MIDIBN_MIDI_STREAM *) streamHandle;
    S_VOICE_MGR *pVoiceMgr;
    S_SYNTH *pSynth;
    S_DLS *pOld;
    EAS_INT i;

    if (pEASData == NULL || pStream == NULL || pStream->pSynth == NULL)
        return EAS_ERROR_PARAMETER_RANGE;
    pVoiceMgr = pEASData->pVoiceMgr;
    pSynth = pStream->pSynth;

    /* free every voice of this stream (the way VMReset(force) does) */
    pVoiceMgr->activeVoices = (EAS_U16) (pVoiceMgr->activeVoices - pSynth->numActiveVoices);
    pSynth->numActiveVoices = 0;
    VMInitializeAllVoices(pVoiceMgr, pSynth->vSynthNum);
    for (i = 0; i < NUM_SYNTH_CHANNELS; i++)
        pSynth->poolCount[i] = 0;

    pOld = pSynth->pDLS;
    pSynth->pDLS = (S_DLS *) dls;
    if (pOld != NULL)
        DLSCleanup(pEASData->hwInstData, pOld);

    /* look the instruments of every channel up again, in the new bank */
    for (i = 0; i < NUM_SYNTH_CHANNELS; i++)
        VMProgramChange(pVoiceMgr, pSynth, (EAS_U8) i, pSynth->channels[i].programNum);
    return EAS_SUCCESS;
}

/* Sonivox's master volume (1 dB steps, 0 to 100; the library starts at 90), applied before the final saturation. */
EAS_RESULT MIDIBN_SetMasterVolume (EAS_DATA_HANDLE pEASData, EAS_I32 volume)
{
    return EAS_SetVolume(pEASData, NULL, volume);
}
