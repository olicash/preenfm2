/*
 * Copyright 2013 Xavier Hosxe
 *
 * Author: Xavier Hosxe (xavier . hosxe (at) gmail . com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdlib.h>
#include "Synth.h"
#include "Menu.h"
#include "stm32f4xx_rng.h"


#include "hardware/dwt.h"

#ifdef DEBUG_CPU_USAGE
CYCCNT_buffer cycles_all;
#endif

extern float frequency[];
extern float noise[32];

// Initialized in main depending of PCB version
float ratiosTimbre[5];
float middleSample;


Synth::Synth(void) {
}

Synth::~Synth(void) {
}

void Synth::init(SynthState* sState) {
    int numberOfVoices[]= { 6, 0, 0, 0 };
    for (int t=0; t<NUMBER_OF_TIMBRES; t++) {
        for (int k=0; k<sizeof(struct OneSynthParams)/sizeof(float); k++) {
            ((float*)&timbres[t].params)[k] = ((float*)&preenMainPreset)[k];
        }
        timbres[t].params.engine1.numberOfVoice = numberOfVoices[t];
        timbres[t].init(t, sState);
        for (int v=0; v<MAX_NUMBER_OF_VOICES; v++) {
            timbres[t].initVoicePointer(v, &voices[v]);
        }
    }

    newTimbre(0);
    this->writeCursor = 0;
    this->readCursor = 0;
    for (int k = 0; k < MAX_NUMBER_OF_VOICES; k++) {
        voices[k].init();
    }
    rebuidVoiceTimbre();
    for (int t=0; t<NUMBER_OF_TIMBRES; t++) {
        timbres[t].numberOfVoicesChanged();
    }
    updateNumberOfActiveTimbres();

#ifdef CVIN
    cvin12Ready = true;
    cvin34Ready = true;
    triggeredTimbre = 0;
#endif

}

void Synth::setDacNumberOfBits(uint32_t dacNumberOfBits) {
    // Usefull to laverage the number of dac bits in the final stage of sample calculation
    middleSample = (1 << (dacNumberOfBits - 1)) - 1;
    ratiosTimbre[0] = (1 << (dacNumberOfBits - 1)) - 1;
    for (int t = 1; t <= 4; t++) {
        ratiosTimbre[t] = ratiosTimbre[0] / t;
    }
}


void Synth::noteOn(int timbre, char note, char velocity) {
    timbres[timbre].noteOn(note, velocity);

}

void Synth::noteOff(int timbre, char note) {
    timbres[timbre].noteOff(note);
}

void Synth::setHoldPedal(int timbre, int value) {
    timbres[timbre].setHoldPedal(value);
}

void Synth::allNoteOff(int timbre) {
    int numberOfVoices = timbres[timbre].params.engine1.numberOfVoice;
    for (int k = 0; k < numberOfVoices; k++) {
        // voice number k of timbre
        int n = timbres[timbre].voiceNumber[k];
        if (voices[n].isPlaying() && !voices[n].isReleased()) {
            voices[n].noteOff();
        }
    }
}

void Synth::allSoundOff(int timbre) {
    int numberOfVoices = timbres[timbre].params.engine1.numberOfVoice;
    for (int k = 0; k < numberOfVoices; k++) {
        // voice number k of timbre
        int n = timbres[timbre].voiceNumber[k];
        voices[n].killNow();
    }
}

void Synth::allSoundOff() {
    for (int k = 0; k < MAX_NUMBER_OF_VOICES; k++) {
        voices[k].killNow();
    }
}

bool Synth::isPlaying() {
    for (int k = 0; k < MAX_NUMBER_OF_VOICES; k++) {
        if (voices[k].isPlaying()) {
            return true;
        }
    }
    return false;
}


#ifdef DEBUG_CPU_USAGE
int cptDisplay = 0;
float totalCycles = 0;
#endif



void Synth::buildNewSampleBlockCS4344(int32_t *sample) {
    CYCLE_MEASURE_START(cycles_all);

    buildNewSampleBlock();
    
    const float *sampleFromTimbre1 = timbres[0].getSampleBlock();
    const float *sampleFromTimbre2 = timbres[1].getSampleBlock();
    const float *sampleFromTimbre3 = timbres[2].getSampleBlock();
    const float *sampleFromTimbre4 = timbres[3].getSampleBlock();

    int32_t *cb = (int32_t*)sample;
    int32_t *cbFin = (int32_t*)&sample[64];

    while (cb != cbFin) {
        int32_t tmpV = *sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++;
        *cb++ = ((tmpV >> 8) + ((tmpV & 0x000000ff) << 24));
        tmpV = *sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++;
        *cb++ = ((tmpV >> 8) + ((tmpV & 0x000000ff) << 24));
        tmpV = *sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++;
        *cb++ = ((tmpV >> 8) + ((tmpV & 0x000000ff) << 24));
        tmpV = *sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++;
        *cb++ = ((tmpV >> 8) + ((tmpV & 0x000000ff) << 24));
    }

    CYCLE_MEASURE_END();

#ifdef DEBUG_CPU_USAGE
    if (cptDisplay++ > 500) {
        totalCycles += cycles_all.remove();

        if (cptDisplay == 600) {
            float max = SystemCoreClock * 32.0f * PREENFM_FREQUENCY_INVERSED;
            cpuUsage = totalCycles / max;
            if (cpuUsage > 95) {
                for (int v = 0; v < MAX_NUMBER_OF_VOICES; v++) {
                    this->voices[v].noteOff();
                }
            }
            cptDisplay = 0;
            totalCycles = 0;
        }
    }
#endif

}


void Synth::buildNewSampleBlockMcp4922() {
    CYCLE_MEASURE_START(cycles_all);
    buildNewSampleBlock();

    uint32_t *sample = &samples[writeCursor];
    const float *sampleFromTimbre1 = timbres[0].getSampleBlock();
    const float *sampleFromTimbre2 = timbres[1].getSampleBlock();
    const float *sampleFromTimbre3 = timbres[2].getSampleBlock();
    const float *sampleFromTimbre4 = timbres[3].getSampleBlock();

    uint32_t *cb = sample;
    uint32_t *cbFin = &sample[64];

    while (cb != cbFin) {
        *cb++ = (int)((*sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++) + middleSample);
        *cb++ = (int)((*sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++) + middleSample);
        *cb++ = (int)((*sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++) + middleSample);
        *cb++ = (int)((*sampleFromTimbre1++ + *sampleFromTimbre2++ + *sampleFromTimbre3++ + *sampleFromTimbre4++) + middleSample);
    }
    writeCursor = (writeCursor + 64) & 255;

    CYCLE_MEASURE_END();

#ifdef DEBUG_CPU_USAGE

    if (cptDisplay++ > 500) {
        totalCycles += cycles_all.remove();

        if (cptDisplay == 600) {
            float max = SystemCoreClock * 32.0f * PREENFM_FREQUENCY_INVERSED;
            cpuUsage = totalCycles / max;
            cptDisplay = 0;
            totalCycles = 0;
        }
    }
#endif


}


void Synth::buildNewSampleBlock() {

    // We consider the random number is always ready here...
    uint32_t random32bit = RNG_GetRandomNumber();
    noise[0] =  (random32bit & 0xffff) * .000030518f - 1.0f; // value between -1 and 1.
    noise[1] = (random32bit >> 16) * .000030518f - 1.0f; // value between -1 and 1.
    for (int noiseIndex = 2; noiseIndex<32; ) {
        random32bit = 214013 * random32bit + 2531011;
        noise[noiseIndex++] =  (random32bit & 0xffff) * .000030518f - 1.0f; // value between -1 and 1.
        noise[noiseIndex++] = (random32bit >> 16) * .000030518f - 1.0f; // value between -1 and 1.
    }

#ifdef CVIN
    cvin->updateValues();
#endif

    for (int t = 0; t < NUMBER_OF_TIMBRES; t++) {
        timbres[t].cleanNextBlock();
        if (likely(timbres[t].params.engine1.numberOfVoice > 0)) {
            timbres[t].prepareForNextBlock();
            // need to glide ?
            if (timbres[t].voiceNumber[0] != -1 && this->voices[timbres[t].voiceNumber[0]].isGliding()) {
                this->voices[timbres[t].voiceNumber[0]].glide();
            }
        }

#ifdef CVIN
        timbres[t].setMatrixSource(MATRIX_SOURCE_CVIN1, cvin->getCvin1());
        timbres[t].setMatrixSource(MATRIX_SOURCE_CVIN2, cvin->getCvin2());
        timbres[t].setMatrixSource(MATRIX_SOURCE_CVIN3, cvin->getCvin3());
        timbres[t].setMatrixSource(MATRIX_SOURCE_CVIN4, cvin->getCvin4());
#endif
        timbres[t].prepareMatrixForNewBlock();
    }


#ifdef CVIN
    // We need matrix source in osc
    // cvin1 can trigger Instrument 1 notes
    int cvinstrument = synthState->fullState.midiConfigValue[MIDICONFIG_CVIN1_2];
    if (cvinstrument >= 0) {
        int timbreIndex = 0;
        int timbreToTrigger[4];
        switch (cvinstrument) {
            case 1:
                timbreToTrigger[timbreIndex++] = 0;
            break;
            case 2:
                timbreToTrigger[timbreIndex++] = 1;
            break;
            case 3:
                timbreToTrigger[timbreIndex++] = 2;
            break;
            case 4: 
                timbreToTrigger[timbreIndex++] = 3;
            break;
            case 5:
                timbreToTrigger[timbreIndex++] = 0;
                timbreToTrigger[timbreIndex++] = 1;
            break;
            case 6: 
                timbreToTrigger[timbreIndex++] = 0;
                timbreToTrigger[timbreIndex++] = 1;
                timbreToTrigger[timbreIndex++] = 2;
            break;
            case 7:
                timbreToTrigger[timbreIndex++] = 0;
                timbreToTrigger[timbreIndex++] = 1;
                timbreToTrigger[timbreIndex++] = 2;
                timbreToTrigger[timbreIndex++] = 3;
            break;
            case 8:
                timbreToTrigger[timbreIndex++] = triggeredTimbre;
            break;
            case 9:
                timbreToTrigger[timbreIndex++] = (int)((noise[0] + 1.0f) * 2.0f);
            break;
            case 10:
                timbreToTrigger[timbreIndex++] = (int)((cvin->getCvin3() + 1.0f) * 2.0f);
            break;
        }

        // CV_GATE from 0 to 100 => cvGate from 62 to 962. 
        // Which leaves some room for the histeresit algo bellow.
        int cvGate = synthState->fullState.midiConfigValue[MIDICONFIG_CV_GATE] * 9 + 62;
        if (cvin12Ready) {
            if (cvin->getGate() > cvGate) {
                cvin12Ready = false;
                for (int tk = 0; tk < timbreIndex; tk++ ) {
                    timbres[timbreToTrigger[tk]].setCvFrequency(cvin->getFrequency());
                    timbres[timbreToTrigger[tk]].noteOn(128, 127);
                    visualInfo->noteOn(timbreToTrigger[tk], true);
                }
                // inc timbre triggerTimbre if we are in Seq mode
                if (unlikely(cvinstrument == 8)) {
                    triggeredTimbre++;
                    triggeredTimbre &= 0x3;
                }
            }
        } else {
            if (cvin->getGate() > (cvGate + 50)) {
                // Adjust frequency with CVIN2 !!! while gate is on !!
                for (int tk = 0; tk < timbreIndex; tk++ ) {
                    timbres[timbreToTrigger[tk]].setCvFrequency(cvin->getFrequency());
                    timbres[timbreToTrigger[tk]].propagateCvFreq(128);
                }
            } else if (cvin->getGate() < (cvGate - 50)) {
                for (int tk = 0; tk < timbreIndex; tk++ ) {
                    timbres[timbreToTrigger[tk]].noteOff(128);
                }
                cvin12Ready = true;
            }
        }
    }
#endif

    // render all voices in their timbre sample block...
    // 16 voices

    playingNotes = 0;

    for (int v = 0; v < MAX_NUMBER_OF_VOICES; v++) {
        if (likely(this->voices[v].isPlaying())) {
            this->voices[v].nextBlock();
            playingNotes ++;
        }
    }

    // Add timbre per timbre because gate and eventual other effect are per timbre
    if (likely(timbres[0].params.engine1.numberOfVoice > 0)) {
        timbres[0].fxAfterBlock(ratioTimbre);
    }
    if (likely(timbres[1].params.engine1.numberOfVoice > 0)) {
        timbres[1].fxAfterBlock(ratioTimbre);
    }
    if (likely(timbres[2].params.engine1.numberOfVoice > 0)) {
        timbres[2].fxAfterBlock(ratioTimbre);
    }
    if (likely(timbres[3].params.engine1.numberOfVoice > 0)) {
        timbres[3].fxAfterBlock(ratioTimbre);
    }

}

void Synth::beforeNewParamsLoad(int timbre) {
    for (int t=0; t<NUMBER_OF_TIMBRES; t++) {
        timbres[t].resetArpeggiator();
        for (int v=0; v<MAX_NUMBER_OF_VOICES; v++) {
            timbres[t].setVoiceNumber(v, -1);
        }
    }
    // Stop all voices
    allSoundOff();
};


int Synth::getNumberOfFreeVoicesForThisTimbre(int timbre) {
    int maxNumberOfFreeVoice = 0;
    for (int t=0 ; t< NUMBER_OF_TIMBRES; t++) {
        if (t != timbre) {
            maxNumberOfFreeVoice += timbres[t].params.engine1.numberOfVoice;
        }
    }
    maxNumberOfFreeVoice =  MAX_NUMBER_OF_VOICES - maxNumberOfFreeVoice;

    int freeOsc = getNumberOfFreeOscForThisTimbre(timbre);
    int maxNumberOfFreeVoicesWithOperators = freeOsc / algoInformation[(int)timbres[timbre].params.engine1.algo].osc ;

    return maxNumberOfFreeVoicesWithOperators < maxNumberOfFreeVoice ? maxNumberOfFreeVoicesWithOperators : maxNumberOfFreeVoice;
}

void Synth::afterNewParamsLoad(int timbre) {
    // Reset to 0 the number of voice then try to set the right value
    int numberOfVoice = timbres[timbre].params.engine1.numberOfVoice;
    int voicesMax = getNumberOfFreeVoicesForThisTimbre(timbre);
    timbres[timbre].params.engine1.numberOfVoice = numberOfVoice < voicesMax ? numberOfVoice : voicesMax;

    rebuidVoiceTimbre();
    updateNumberOfActiveTimbres();

    timbres[timbre].numberOfVoicesChanged();
    timbres[timbre].afterNewParamsLoad();
    // values to force check lfo used
    timbres[timbre].verifyLfoUsed(ENCODER_MATRIX_SOURCE, 0.0f, 1.0f);

}

void Synth::afterNewComboLoad() {


    // If combo have been saved with 16 voices
    // Reduce number of voices of timbre with more voices
    int timbreMax = -1;
    int voicesOfTimbreMax = -1;
    for (int t=0; t<NUMBER_OF_TIMBRES ; t++) {
        int numberOfVoice = timbres[t].params.engine1.numberOfVoice;
        if (numberOfVoice > voicesOfTimbreMax) {
            timbreMax = t;
            voicesOfTimbreMax = numberOfVoice;
        }
    }
    if (timbreMax >= 0) {
        int voicesMax = getNumberOfFreeVoicesForThisTimbre(timbreMax);
        timbres[timbreMax].params.engine1.numberOfVoice = voicesMax < voicesOfTimbreMax ? voicesMax : voicesOfTimbreMax;
    }

    rebuidVoiceTimbre();
    updateNumberOfActiveTimbres();

    for (int t=0; t<NUMBER_OF_TIMBRES ; t++) {
        timbres[t].numberOfVoicesChanged();
        timbres[t].afterNewParamsLoad();
        // values to force check lfo used
        timbres[t].verifyLfoUsed(ENCODER_MATRIX_SOURCE, 0.0f, 1.0f);
        //
    }
}

void Synth::updateNumberOfActiveTimbres() {
    int activeTimbres = 0;
    if (timbres[0].params.engine1.numberOfVoice > 0) {
        activeTimbres ++;
    }
    if (timbres[1].params.engine1.numberOfVoice > 0) {
        activeTimbres ++;
    }
    if (timbres[2].params.engine1.numberOfVoice > 0) {
        activeTimbres ++;
    }
    if (timbres[3].params.engine1.numberOfVoice > 0) {
        activeTimbres ++;
    }
    ratioTimbre = ratiosTimbre[activeTimbres];
}

int Synth::getFreeVoice() {
    // Loop on all voices
    for (int voice=0; voice< MAX_NUMBER_OF_VOICES; voice++) {
        bool used = false;

        for (int t=0; t< NUMBER_OF_TIMBRES && !used; t++) {
            // Must be different from 0 and -1
            int interVoice = -10;
            for (int v=0;  v < MAX_NUMBER_OF_VOICES  && !used; v++) {
                interVoice = timbres[t].voiceNumber[v];
                if (interVoice == -1) {
                    break;
                }
                if (interVoice == voice) {
                    used = true;
                }
            }
        }

        if (!used) {
            return voice;
        }
    }
    return -1;
}

// can prevent some value change...
void Synth::checkNewParamValue(int timbre, int currentRow, int encoder, float oldValue, float *newValue) {
    if (unlikely(currentRow == ROW_ENGINE)) {
        if (unlikely(encoder == ENCODER_ENGINE_VOICE)) {
            // Increase number of voice ?
            if ((*newValue)> oldValue) {
                if ((*newValue) > getNumberOfFreeVoicesForThisTimbre(timbre)) {
                    *newValue = oldValue;
                }
            }
        }
    }
}

void Synth::newParamValue(int timbre, int currentRow, int encoder, ParameterDisplay* param, float oldValue, float newValue) {
    switch (currentRow) {
    case ROW_ENGINE:
        switch (encoder) {
        case ENCODER_ENGINE_ALGO:
            fixMaxNumberOfVoices(timbre);
            timbres[timbre].initADSRloop();
            break;
        case ENCODER_ENGINE_VOICE:
            if (newValue > oldValue) {
                for (int v=(int)oldValue; v < (int)newValue; v++) {
                    timbres[timbre].setVoiceNumber(v, getFreeVoice());
                }
            } else {
                for (int v=(int)newValue; v < (int)oldValue; v++) {
                    voices[timbres[timbre].voiceNumber[v]].killNow();
                    timbres[timbre].setVoiceNumber(v, -1);
                }
            }
            timbres[timbre].numberOfVoicesChanged();
            if (newValue == 0.0f || oldValue == 0.0f) {
                updateNumberOfActiveTimbres();
            }
            break;
        }
        break;
    case ROW_ARPEGGIATOR1:
        switch (encoder) {
        case ENCODER_ARPEGGIATOR_CLOCK:
            allNoteOff(timbre);
            timbres[timbre].setArpeggiatorClock((uint8_t) newValue);
            break;
        case ENCODER_ARPEGGIATOR_BPM:
            timbres[timbre].setNewBPMValue((uint8_t) newValue);
            break;
        case ENCODER_ARPEGGIATOR_DIRECTION:
            timbres[timbre].setDirection((uint8_t) newValue);
            break;
        }
        break;
    case ROW_ARPEGGIATOR2:
        if (unlikely(encoder == ENCODER_ARPEGGIATOR_LATCH)) {
            timbres[timbre].setLatchMode((uint8_t) newValue);
        }
        break;
    case ROW_ARPEGGIATOR3:
        break;
    case ROW_EFFECT:
        timbres[timbre].setNewEffecParam(encoder);
        break;
    case ROW_ENV1a:
        timbres[timbre].env1.reloadADSR(encoder);
        break;
    case ROW_ENV1b:
        timbres[timbre].env1.reloadADSR(encoder + 4);
        break;
    case ROW_ENV2a:
        timbres[timbre].env2.reloadADSR(encoder);
        break;
    case ROW_ENV2b:
        timbres[timbre].env2.reloadADSR(encoder + 4);
        break;
    case ROW_ENV3a:
        timbres[timbre].env3.reloadADSR(encoder);
        break;
    case ROW_ENV3b:
        timbres[timbre].env3.reloadADSR(encoder + 4);
        break;
    case ROW_ENV4a:
        timbres[timbre].env4.reloadADSR(encoder);
        break;
    case ROW_ENV4b:
        timbres[timbre].env4.reloadADSR(encoder + 4);
        break;
    case ROW_ENV5a:
        timbres[timbre].env5.reloadADSR(encoder);
        break;
    case ROW_ENV5b:
        timbres[timbre].env5.reloadADSR(encoder + 4);
        break;
    case ROW_ENV6a:
        timbres[timbre].env6.reloadADSR(encoder);
        break;
    case ROW_ENV6b:
        timbres[timbre].env6.reloadADSR(encoder + 4);
        break;
    case ROW_MATRIX_FIRST ... ROW_MATRIX_LAST:
        timbres[timbre].verifyLfoUsed(encoder, oldValue, newValue);
        if (encoder == ENCODER_MATRIX_DEST1 || encoder == ENCODER_MATRIX_DEST2) {
            timbres[timbre].resetMatrixDestination(oldValue);
        }
        break;
    case ROW_LFOOSC1 ... ROW_LFOOSC3:
    case ROW_LFOENV1 ... ROW_LFOENV2:
    case ROW_LFOSEQ1 ... ROW_LFOSEQ2:
        // timbres[timbre].lfo[currentRow - ROW_LFOOSC1]->valueChanged(encoder);
        timbres[timbre].lfoValueChange(currentRow, encoder, newValue);
        break;
    case ROW_PERFORMANCE1:
        timbres[timbre].setMatrixSource((enum SourceEnum)(MATRIX_SOURCE_CC1 + encoder), newValue);
        break;
    case ROW_MIDINOTE1CURVE:
        timbres[timbre].updateMidiNoteScale(0);
        break;
    case ROW_MIDINOTE2CURVE:
        timbres[timbre].updateMidiNoteScale(1);
        break;
    }
}


// synth is the only one who knows timbres
void Synth::newTimbre(int timbre)  {
    this->synthState->setParamsAndTimbre(&timbres[timbre].params, timbre);
}


/*
 * Return false if had to change number of voice
 *
 */
bool Synth::fixMaxNumberOfVoices(int timbre) {
    int voicesMax = getNumberOfFreeVoicesForThisTimbre(timbre) ;

    if (this->timbres[timbre].params.engine1.numberOfVoice > voicesMax) {
        int oldValue = this->timbres[timbre].params.engine1.numberOfVoice;
        this->timbres[timbre].params.engine1.numberOfVoice = voicesMax;
        ParameterDisplay *params = &allParameterRows.row[ROW_ENGINE]->params[ENCODER_ENGINE_VOICE];
        synthState->propagateNewParamValue(timbre, ROW_ENGINE, ENCODER_ENGINE_VOICE, params, oldValue, voicesMax );
        return false;
    }

    return true;
}



void Synth::rebuidVoiceTimbre() {
    int voices = 0;

    for (int t=0; t<NUMBER_OF_TIMBRES; t++) {
        int nv = timbres[t].params.engine1.numberOfVoice;

        for (int v=0; v < nv; v++) {
            timbres[t].setVoiceNumber(v, voices++);
        }
        for (int v=nv; v < MAX_NUMBER_OF_VOICES;  v++) {
            timbres[t].setVoiceNumber(v, -1);
        }
    }
}

int Synth::getNumberOfFreeOscForThisTimbre(int timbre) {
    int numberOfOsc = 0;

    for (int t=0; t<NUMBER_OF_TIMBRES; t++) {
        if (t != timbre) {
            int nv = timbres[t].params.engine1.numberOfVoice + .0001f;
            numberOfOsc += algoInformation[(int)timbres[t].params.engine1.algo].osc * nv;
        }
    }

    return MAX_NUMBER_OF_OPERATORS - numberOfOsc;
}

void Synth::loadPreenFMPatchFromMidi(int timbre, int bank, int bankLSB, int patchNumber) {
    this->synthState->loadPreenFMPatchFromMidi(timbre, bank, bankLSB, patchNumber, &timbres[timbre].params);
}


void Synth::setNewValueFromMidi(int timbre, int row, int encoder, float newValue) {
    struct ParameterDisplay* param = &(allParameterRows.row[row]->params[encoder]);
    int index = row * NUMBER_OF_ENCODERS + encoder;
    float oldValue = ((float*)this->timbres[timbre].getParamRaw())[index];

    // 2.08e : FIX CRASH when sending to many number of voices from editor !!!!
    if (unlikely(row == ROW_ENGINE)) {
        if (unlikely(encoder == ENCODER_ENGINE_VOICE)) {
            int maxNumberOfVoices = getNumberOfFreeVoicesForThisTimbre(timbre);
            if (newValue > maxNumberOfVoices) {
                newValue = maxNumberOfVoices;
            }
        }
    }


    this->timbres[timbre].setNewValue(index, param, newValue);
    float newNewValue = ((float*)this->timbres[timbre].getParamRaw())[index];
    if (oldValue != newNewValue) {
        this->synthState->propagateNewParamValueFromExternal(timbre, row, encoder, param, oldValue, newNewValue);
    }
}

void Synth::setNewStepValueFromMidi(int timbre, int whichStepSeq, int step, int newValue) {

    if (whichStepSeq < 0 || whichStepSeq > 1 || step < 0 || step > 15 || newValue < 0 || newValue > 15) {
        return;
    }

    int oldValue = this->timbres[timbre].getSeqStepValue(whichStepSeq, step);

    if (oldValue !=  newValue) {
        int oldStep = this->synthState->stepSelect[whichStepSeq];
        this->synthState->stepSelect[whichStepSeq] = step;
        if (oldStep != step) {
            this->synthState->propagateNewParamValueFromExternal(timbre, ROW_LFOSEQ1 + whichStepSeq, 2, NULL, oldStep, step);
        }
        this->timbres[timbre].setSeqStepValue(whichStepSeq, step, newValue);
        int newNewValue = this->timbres[timbre].getSeqStepValue(whichStepSeq, step);
        if (oldValue != newNewValue) {
            this->synthState->propagateNewParamValueFromExternal(timbre, ROW_LFOSEQ1 + whichStepSeq, 3, NULL, oldValue, newValue);
        }
    }
}



void Synth::setNewSymbolInPresetName(int timbre, int index, int value) {
    this->timbres[timbre].getParamRaw()->presetName[index] = value;
}



void Synth::setScalaEnable(bool enable) {
    this->synthState->setScalaEnable(enable);
}

void Synth::setScalaScale(int scaleNumber) {
    this->synthState->setScalaScale(scaleNumber);    
}

void Synth::setCurrentInstrument(int value) {
    if (value >=1 && value <= 4) {
        this->synthState->setCurrentInstrument(value);    
    }
}

bool Synth::analyseSysexBuffer(uint8_t *buffer,int len) {
    
    if (buffer[0]!=0x7d && (buffer[0]==0x7e || buffer[0]==0x7f)) {
        for (int i=0;i<16;i++) updatedNotes[i]=0;
        if (decodeBufferAndApplyTuning(buffer, len, this->updatedNotes)) {
            for (int i=0;i<MAX_NUMBER_OF_VOICES;i++) voices[i].updateOscillatorTunings(this->updatedNotes);
        }
        return true;
    }
    return false;
}

// returns true if frequency of any notes have changed
bool Synth::decodeBufferAndApplyTuning(const unsigned char *buffer,int len,unsigned char *updatedNotes) {
    bool ret=false;
    int sysex_ctr=0,sysex_value=0,note=0,numTunings=0;
    int bank=-1,prog=0,checksum=0;short int channelBitmap=0;   // maybe we'll want to use these at some point
    eSysexState state=eMatchingSysex;eMTSFormat format=eBulk;bool realtime=false;
    for (int i=0;buffer[i]!=0xF7;i++)
    {
        unsigned char b=buffer[i];
        if (b==0xF7) {state=eIgnoring;continue;}
        switch (state)
        {
            case eIgnoring:
                if (b==0xF0) state=eMatchingSysex;
                break;
            case eMatchingSysex:
                sysex_ctr=0;
                if (b==0x7E) state=eSysexValid;
                else if (b==0x7F) realtime=true,state=eSysexValid;
                else state=eMatchingSysex;   // don't switch to ignoring...Scala adds two bytes here, we need to skip over them
                break;
            case eSysexValid:
                switch (sysex_ctr++)    // handle device ID
                {
                    case 0: if (b!=0x00 && b!=0x7F) state=eIgnoring; break;
                    case 1: if (b==0x08) state=eMatchingMTS; break; // no extended device IDs have byte 2 set to 0x08, so this is a safe check for MTS message
                    default: state=eIgnoring; break;    // it's not an MTS message
                }
                break;
            case eMatchingMTS:
                sysex_ctr=0;
                // ret=true;   // assume we've got a valid MTS message
                switch (b)
                {
                    case 0: format=eRequest,state=eMatchingProg; break;
                    case 1: format=eBulk,state=eMatchingProg; break;
                    case 2: format=eSingle,state=eMatchingProg; break;
                    case 3: format=eRequest,state=eMatchingBank; break;
                    case 4: format=eBulk,state=eMatchingBank; break;
                    case 5: format=eScaleOctOneByte,state=eMatchingBank; break;
                    case 6: format=eScaleOctTwoByte,state=eMatchingBank; break;
                    case 7: format=eSingle,state=eMatchingBank; break;
                    case 8: format=eScaleOctOneByteExt,state=eMatchingChannel; break;
                    case 9: format=eScaleOctTwoByteExt,state=eMatchingChannel; break;
                    default: state=eIgnoring; break;    // it's not a valid MTS format
                }
                break;
            case eMatchingBank:
                bank=b;
                state=eMatchingProg;
                break;
            case eMatchingProg:
                prog=b;
                if (format==eSingle) state=eNumTunings;else state=eTuningName;//,tuningName.clear();
                break;
            case eTuningName:
//                tuningName.push_back(b);
                if (++sysex_ctr>=16) sysex_ctr=0,state=eTuningData;
                break;
            case eNumTunings:
                numTunings=b&127,sysex_ctr=0,state=eTuningData;
                break;
            case eMatchingChannel:
                switch (sysex_ctr++)
                {
                    case 0: for (int i=14;i<16;i++) channelBitmap|=(1<<i); break;
                    case 1: for (int i=7;i<14;i++) channelBitmap|=(1<<i); break;
                    case 2: for (int i=0;i<7;i++) channelBitmap|=(1<<i); sysex_ctr=0,state=eTuningData; break;
                }
                break;
            case eTuningData:
                switch (format)
                {
                    case eBulk:
                        sysex_value=(sysex_value<<7)|(b&127);
                        sysex_ctr++;
                        if ((sysex_ctr&3)==3)
                        {
                            if (!(note==0x7F && sysex_value==16383) && retuneNote(note,(sysex_value>>14)&127,(sysex_value&16383)/16383.f)) {updatedNotes[note>>3]|=1<<(note&7);ret=true;}
                            sysex_value=0;sysex_ctr++;
                            if (++note>=128) state=eCheckSum;
                        }
                        break;
                    case eSingle:
                        sysex_value=(sysex_value<<7)|(b&127);
                        sysex_ctr++;
                        if (!(sysex_ctr&3))
                        {
                            char n=(sysex_value>>21)&127;
                            if (!(note==0x7F && sysex_value==16383) && retuneNote(n,(sysex_value>>14)&127,(sysex_value&16383)/16383.f)) {updatedNotes[n>>3]|=1<<(n&7);ret=true;}
                            sysex_value=0;
                            if (++note>=numTunings) state=eIgnoring;
                        }
                        break;
                    case eScaleOctOneByte: case eScaleOctOneByteExt:
                        for (int i=sysex_ctr;i<128;i+=12) if (retuneNote(i,i,((b&127)-64.f)*0.01f)) {updatedNotes[i>>3]|=1<<(i&7);ret=true;}
                        if (++sysex_ctr>=12) state=format==eScaleOctOneByte?eCheckSum:eIgnoring;
                        break;
                    case eScaleOctTwoByte: case eScaleOctTwoByteExt:
                        sysex_value=(sysex_value<<7)|(b&127);
                        sysex_ctr++;
                        if (!(sysex_ctr&1))
                        {
                            float detune=((float)(sysex_value&16383)-8192.f)/(sysex_value>8192.f?8191.f:8192.f);
                            for (int i=note;i<128;i+=12) if (retuneNote(i,i,detune)) {updatedNotes[i>>3]|=1<<(i&7);ret=true;}
                            if (++note>=12) state=format==eScaleOctTwoByte?eCheckSum:eIgnoring;
                        }
                        break;
                    default: state=eIgnoring; break;
                }
                break;
            case eCheckSum:
                checksum=b;
                state=eIgnoring;
                break;
        }
    }
    return ret;
}

bool Synth::retuneNote(char note,char retuneNote,float detune) {
    float old=frequency[note];
    frequency[note]=440.f*pow(2.f,((retuneNote+detune)-69.f)/12.f);
    return old!=frequency[note];
}

void Synth::newMenuSelect(FullState* fullState) {
    if (fullState->currentMenuItem->menuState == MENU_CONFIG_SETTINGS 
         && midiConfig[fullState->menuSelect].valueName != NULL
         && midiConfig[fullState->menuSelect].valueName[0][0] == 'G') {
            updateGlobalTuningFromConfig();
    }
#ifdef CVIN      
    if (fullState->currentMenuItem->menuState == MENU_CONFIG_SETTINGS) {
        if (fullState->menuSelect == MIDICONFIG_CVIN_A2 || fullState->menuSelect == MIDICONFIG_CVIN_A6) {
            cvin->updateFormula(synthState->fullState.midiConfigValue[MIDICONFIG_CVIN_A2], synthState->fullState.midiConfigValue[MIDICONFIG_CVIN_A6]);
        }
    }
#endif
}

void Synth::updateGlobalTuningFromConfig() {
    synthState->fullState.globalTuning = 430.0f + .2f * synthState->fullState.midiConfigValue[MIDICONFIG_GLOBAL_TUNING];
}


#ifdef DEBUG

// ========================== DEBUG ========================
void Synth::debugVoice() {

    lcd.setRealTimeAction(true);
    lcd.clearActions();
    lcd.clear();
    int numberOfVoices = timbres[0].params.engine1.numberOfVoice;
    // HARDFAULT !!! :-)
    //    for (int k = 0; k <10000; k++) {
    //    	numberOfVoices += timbres[k].params.engine1.numberOfVoice;
    //    	timbres[k].params.engine1.numberOfVoice = 100;
    //    }

    for (int k = 0; k < numberOfVoices && k < 4; k++) {

        // voice number k of timbre
        int n = timbres[0].voiceNumber[k];

        lcd.setCursor(0, k);
        lcd.print((int)voices[n].getNote());

        lcd.setCursor(4, k);
        lcd.print((int)voices[n].getNextPendingNote());

        lcd.setCursor(8, k);
        lcd.print(n);

        lcd.setCursor(12, k);
        lcd.print((int)voices[n].getIndex());

        lcd.setCursor(18, k);
        lcd.print((int) voices[n].isReleased());
        lcd.print((int) voices[n].isPlaying());
    }
    lcd.setRealTimeAction(false);
}

void Synth::showCycles() {
    lcd.setRealTimeAction(true);
    lcd.clearActions();
    lcd.clear();

    float max = SystemCoreClock * 32.0f * PREENFM_FREQUENCY_INVERSED;
    int cycles = cycles_all.remove();
    float percent = (float)cycles * 100.0f / max;
    lcd.setCursor(10, 0);
    lcd.print('>');
    lcd.print(cycles);
    lcd.print('<');
    lcd.setCursor(10, 1);
    lcd.print('>');
    lcd.print(percent);
    lcd.print('%');
    lcd.print('<');

    /*
    lcd.setCursor( 0, 0 );
    lcd.print( "RNG: " );
    lcd.print( cycles_rng.remove() );

    lcd.setCursor( 0, 1 );
    lcd.print( "VOI: " );  lcd.print( cycles_voices1.remove() );
    lcd.print( " " ); lcd.print( cycles_voices2.remove() );

    lcd.setCursor( 0, 2 );
    lcd.print( "FX : " );
    lcd.print( cycles_fx.remove() );

    lcd.setCursor( 0, 3 );
    lcd.print( "TIM: " );
    lcd.print( cycles_timbres.remove() );

    lcd.setRealTimeAction(false);
     */
}

#endif
