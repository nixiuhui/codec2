/*---------------------------------------------------------------------------*\

  FILE........: freedv_fsk.c
  AUTHOR......: David Rowe
  DATE CREATED: May 2020

  Functions that implement the FreeDV modes that use the FSK modem.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "fsk.h"
#include "fmfsk.h"
#include "codec2.h"
#include "freedv_vhf_framing.h"
#include "varicode.h"
#include "freedv_api.h"
#include "freedv_api_internal.h"
#include "comp_prim.h"
#include "debug_alloc.h"
#include "ldpc_codes.h"
#include "interldpc.h"

/* 16 bit 0x5186 Unique word for fsk_ldpc modes */
static uint8_t fsk_ldpc_uw[] = {0,1,0,1, 0,0,0,1, 1,0,0,0, 0,1,1,0}; 
    
void freedv_2400a_open(struct freedv *f) {
    f->n_protocol_bits = 20;
    f->deframer = fvhff_create_deframer(FREEDV_VHF_FRAME_A,0);
    assert(f->deframer != NULL);  
    f->fsk = fsk_create_hbr(48000,1200,4,10,FSK_DEFAULT_NSYM,1200,1200);
    assert(f->fsk != NULL);
        
    /* Note: fsk expects tx/rx bits as an array of uint8_ts, not ints */
    f->tx_bits = (int*)MALLOC(f->fsk->Nbits*sizeof(uint8_t));        
    assert(f->tx_bits != NULL);

    f->n_nom_modem_samples = f->fsk->N;
    f->n_max_modem_samples = f->fsk->N + (f->fsk->Ts);
    f->n_nat_modem_samples = f->fsk->N;
    f->nin = f->nin_prev = fsk_nin(f->fsk);
    f->modem_sample_rate = 48000;
    f->modem_symbol_rate = 1200;

    f->speech_sample_rate = FREEDV_FS_8000;
    f->codec2 = codec2_create(CODEC2_MODE_1300); assert(f->codec2 != NULL);
    f->n_speech_samples = codec2_samples_per_frame(f->codec2);

    f->n_codec_frames = 1;
    f->bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
    f->bits_per_modem_frame = f->bits_per_codec_frame;
    int n_packed_bytes = (f->bits_per_modem_frame + 7)/8;
    f->tx_payload_bits = MALLOC(n_packed_bytes); assert(f->tx_payload_bits != NULL);  
    f->rx_payload_bits = MALLOC(n_packed_bytes); assert(f->rx_payload_bits != NULL);  
}

void freedv_2400b_open(struct freedv *f) {
    f->n_protocol_bits = 20;
    f->deframer = fvhff_create_deframer(FREEDV_VHF_FRAME_A,1);
    assert(f->deframer != NULL);
        
    f->fmfsk = fmfsk_create(48000,2400);
    assert (f->fmfsk != NULL);

    /* Note: fsk expects tx/rx bits as an array of uint8_ts, not ints */
    f->tx_bits = (int*)MALLOC(f->fmfsk->nbit*sizeof(uint8_t));
    assert(f->tx_bits != NULL);
    f->n_nom_modem_samples = f->fmfsk->N;
    f->n_max_modem_samples = f->fmfsk->N + (f->fmfsk->Ts);
    f->n_nat_modem_samples = f->fmfsk->N;
    f->nin = f->nin_prev = fmfsk_nin(f->fmfsk);
    f->modem_sample_rate = 48000;

    f->speech_sample_rate = FREEDV_FS_8000;
    f->codec2 = codec2_create(CODEC2_MODE_1300); assert(f->codec2 != NULL);
    f->n_speech_samples = codec2_samples_per_frame(f->codec2);

    f->n_codec_frames = 1;
    f->bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
    f->bits_per_modem_frame = f->bits_per_codec_frame;
    int n_packed_bytes = (f->bits_per_modem_frame + 7)/8;
    f->tx_payload_bits = MALLOC(n_packed_bytes); assert(f->tx_payload_bits != NULL);  
    f->rx_payload_bits = MALLOC(n_packed_bytes); assert(f->rx_payload_bits != NULL);  
}

void freedv_800xa_open(struct freedv *f) {
    f->deframer = fvhff_create_deframer(FREEDV_HF_FRAME_B,0);
    assert(f->deframer != NULL);
    f->fsk = fsk_create_hbr(8000,400,4,10,32,800,400);
    assert(f->fsk != NULL);
        
    f->tx_bits = (int*)MALLOC(f->fsk->Nbits*sizeof(uint8_t));
    assert(f->fsk != NULL);
        
    f->n_nom_modem_samples = f->fsk->N;
    f->n_max_modem_samples = f->fsk->N + (f->fsk->Ts);
    f->n_nat_modem_samples = f->fsk->N;
    f->nin = f->nin_prev = fsk_nin(f->fsk);
    f->modem_sample_rate = 8000;
    f->modem_symbol_rate = 400;
    fsk_stats_normalise_eye(f->fsk, 0);

    f->codec2 = codec2_create(CODEC2_MODE_700C); assert(f->codec2 != NULL);
    f->speech_sample_rate = FREEDV_FS_8000;
    f->n_codec_frames = 2;
    f->n_speech_samples = f->n_codec_frames*codec2_samples_per_frame(f->codec2);

    f->bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
    f->bits_per_modem_frame = f->n_codec_frames*f->bits_per_codec_frame;
    int n_packed_bytes = (f->bits_per_modem_frame + 7)/8;
    f->tx_payload_bits = MALLOC(n_packed_bytes); assert(f->tx_payload_bits != NULL);  
    f->rx_payload_bits = MALLOC(n_packed_bytes); assert(f->rx_payload_bits != NULL);  
}


void freedv_fsk_ldpc_open(struct freedv *f, struct freedv_advanced *adv) {
    assert(adv != NULL);

    /* set up modem */
    assert((adv->Fs % adv->Rs) == 0);  // Fs/Rs must be an integer
    int P = adv->Fs/adv->Rs;
    assert(P >= 8);                    // Good idea for P >= 8
    while ((P > 10) && ((P % 2) == 0)) // reduce internal oversampling rate P as far as we can, keep it an integer
        P /= 2;
    //fprintf(stderr, "Fs: %d Rs: %d M: %d P: %d\n", adv->Fs, adv->Rs, adv->M, P);
    f->fsk = fsk_create_hbr(adv->Fs, adv->Rs, adv->M, P, FSK_DEFAULT_NSYM, adv->first_tone, adv->tone_spacing);
    assert(f->fsk != NULL);
    fsk_set_freq_est_limits(f->fsk, 0, adv->Fs/2);
    fsk_stats_normalise_eye(f->fsk, 0);

    /* set up LDPC code */
    int code_index = ldpc_codes_find(adv->codename);
    assert(code_index != -1);
    f->ldpc = (struct LDPC*)MALLOC(sizeof(struct LDPC)); assert(f->ldpc != NULL);
    ldpc_codes_setup(f->ldpc, adv->codename);
    //fprintf(stderr, "Using: %s\n", f->ldpc->name);

    f->bits_per_modem_frame = f->ldpc->data_bits_per_frame;
    int bits_per_frame = f->ldpc->coded_bits_per_frame + sizeof(fsk_ldpc_uw);
    f->tx_payload_bits = malloc(f->bits_per_modem_frame); assert(f->tx_payload_bits != NULL);
    f->rx_payload_bits = malloc(f->bits_per_modem_frame); assert(f->rx_payload_bits != NULL);
    
    /* sample buffer size for tx modem samples, we modulate a full frame */
    f->n_nom_modem_samples = f->fsk->Ts*(bits_per_frame/(f->fsk->mode>>1));
    f->n_nat_modem_samples = f->n_nom_modem_samples;

    /* maximum sample buffer size for rx modem samples, note we only
       demodulate partial frames on each call to fsk_demod() */
    f->n_max_modem_samples = f->fsk->N + (f->fsk->Ts);

    f->nin = f->nin_prev = fsk_nin(f->fsk);
    f->modem_sample_rate = adv->Fs;
    f->modem_symbol_rate = adv->Rs;

    /* deframer set up */
    f->twoframes_hard = malloc(2*bits_per_frame); assert(f->twoframes_hard != NULL);
    memset(f->twoframes_hard, 0, 2*bits_per_frame);
    f->twoframes_llr = (float*)malloc(2*bits_per_frame*sizeof(float)); assert(f->twoframes_llr != NULL);
    for(int i=0; i<2*bits_per_frame; i++) f->twoframes_llr[i] = 0.0;
    f->fsk_ldpc_state = 0;
    f->fsk_ldpc_thresh1 = 0.1*sizeof(fsk_ldpc_uw);
    f->fsk_ldpc_thresh2 = 0.4*sizeof(fsk_ldpc_uw);
    f->fsk_ldpc_baduw = 0;
    //fprintf(stderr, "thresh1: %d thresh2: %d\n", f->fsk_ldpc_thresh1, f->fsk_ldpc_thresh2);
    f->fsk_ldpc_nbits = f->fsk_ldpc_newbits = 0;
    f->fsk_ldpc_best_location = 0;
}


/* TX routines for 2400 FSK modes, after codec2 encoding */
void freedv_tx_fsk_voice(struct freedv *f, short mod_out[]) {
    int  i;
    float *tx_float; /* To hold on to modulated samps from fsk/fmfsk */
    uint8_t vc_bits[2]; /* Varicode bits for 2400 framing */
    uint8_t proto_bits[3]; /* Prococol bits for 2400 framing */

    /* Frame for 2400A/B */
    if(FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_2400B, f->mode)){
        /* Get varicode bits for TX and possibly ask for a new char */
        /* 2 bits per 2400A/B frame, so this has to be done twice */
        for(i=0;i<2;i++){
            if (f->nvaricode_bits) {
                vc_bits[i] = f->tx_varicode_bits[f->varicode_bit_index++];
                f->nvaricode_bits--;
            }

            if (f->nvaricode_bits == 0) {
                /* get new char and encode */
                char s[2];
                if (f->freedv_get_next_tx_char != NULL) {
                    s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
                    f->nvaricode_bits = varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1, 1);
                    f->varicode_bit_index = 0;
                }
            }
        }

        /* If the API user hasn't set up message callbacks, don't bother with varicode bits */
        if(f->freedv_get_next_proto != NULL){
            (*f->freedv_get_next_proto)(f->proto_callback_state,(char*)proto_bits);
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,proto_bits,vc_bits);
        }else if(f->freedv_get_next_tx_char != NULL){
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,vc_bits);
        }else {
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,NULL);
        }
    /* Frame for 800XA */
    }else if(FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){
        fvhff_frame_bits(FREEDV_HF_FRAME_B,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,NULL);
    }

    /* Allocate floating point buffer for FSK mod */
    tx_float = alloca(sizeof(float)*f->n_nom_modem_samples);

    /* do 4fsk mod */
    if(FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){
        if (f->ext_vco) {
            fsk_mod_ext_vco(f->fsk,tx_float,(uint8_t*)(f->tx_bits), f->fsk->Nbits);
            for(i=0; i<f->n_nom_modem_samples; i++){
                mod_out[i] = (short)tx_float[i];
            }
        }
        else {
            fsk_mod(f->fsk,tx_float,(uint8_t*)(f->tx_bits),f->fsk->Nbits);
            /* Convert float samps to short */
            for(i=0; i<f->n_nom_modem_samples; i++){
                mod_out[i] = (short)(tx_float[i]*FSK_SCALE*NORM_PWR_FSK);
            }
        }
    /* do me-fsk mod */
    }else if(FDV_MODE_ACTIVE( FREEDV_MODE_2400B, f->mode)){
        fmfsk_mod(f->fmfsk,tx_float,(uint8_t*)(f->tx_bits));
        /* Convert float samps to short */
        for(i=0; i<f->n_nom_modem_samples; i++){
            mod_out[i] = (short)(tx_float[i]*FMFSK_SCALE);
        }
    }
}

/* TX routines for 2400 FSK modes, after codec2 encoding */
void freedv_comptx_fsk_voice(struct freedv *f, COMP mod_out[]) {
    int  i;
    float *tx_float; /* To hold on to modulated samps from fsk/fmfsk */
    uint8_t vc_bits[2]; /* Varicode bits for 2400 framing */
    uint8_t proto_bits[3]; /* Prococol bits for 2400 framing */

    /* Frame for 2400A/B */
    if(FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_2400B, f->mode)){
        /* Get varicode bits for TX and possibly ask for a new char */
        /* 2 bits per 2400A/B frame, so this has to be done twice */
        for(i=0;i<2;i++){
            if (f->nvaricode_bits) {
                vc_bits[i] = f->tx_varicode_bits[f->varicode_bit_index++];
                f->nvaricode_bits--;
            }

            if (f->nvaricode_bits == 0) {
                /* get new char and encode */
                char s[2];
                if (f->freedv_get_next_tx_char != NULL) {
                    s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
                    f->nvaricode_bits = varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1, 1);
                    f->varicode_bit_index = 0;
                }
            }
        }

        /* If the API user hasn't set up message callbacks, don't bother with varicode bits */
        if(f->freedv_get_next_proto != NULL){
            (*f->freedv_get_next_proto)(f->proto_callback_state,(char*)proto_bits);
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,proto_bits,vc_bits);
        }else if(f->freedv_get_next_tx_char != NULL){
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,vc_bits);
        }else {
            fvhff_frame_bits(FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,NULL);
        }
    /* Frame for 800XA */
    }else if(FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){
        fvhff_frame_bits(FREEDV_HF_FRAME_B,(uint8_t*)(f->tx_bits),f->tx_payload_bits,NULL,NULL);
    }

    /* Allocate floating point buffer for FSK mod */
    tx_float = alloca(sizeof(float)*f->n_nom_modem_samples);

    /* do 4fsk mod */
    if(FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){
        fsk_mod_c(f->fsk,mod_out,(uint8_t*)(f->tx_bits), f->fsk->Nbits);
        /* Convert float samps to short */
        for(i=0; i<f->n_nom_modem_samples; i++){
        	mod_out[i] = fcmult(NORM_PWR_FSK,mod_out[i]);
        }
    /* do me-fsk mod */
    }else if(FDV_MODE_ACTIVE( FREEDV_MODE_2400B, f->mode)){
        fmfsk_mod(f->fmfsk,tx_float,(uint8_t*)(f->tx_bits));
        /* Convert float samps to short */
        for(i=0; i<f->n_nom_modem_samples; i++){
            mod_out[i].real = (tx_float[i]);
        }
    }
}

/* TX routines for 2400 FSK modes, data channel */
void freedv_tx_fsk_data(struct freedv *f, short mod_out[]) {
    int  i;
    float *tx_float; /* To hold on to modulated samps from fsk/fmfsk */
    
    if (FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode))
	fvhff_frame_data_bits(f->deframer, FREEDV_HF_FRAME_B,(uint8_t*)(f->tx_bits));
    else
    	fvhff_frame_data_bits(f->deframer, FREEDV_VHF_FRAME_A,(uint8_t*)(f->tx_bits));
        
    /* Allocate floating point buffer for FSK mod */
    tx_float = alloca(sizeof(float)*f->n_nom_modem_samples);
        
    /* do 4fsk mod */
    if (FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){
        fsk_mod(f->fsk,tx_float,(uint8_t*)(f->tx_bits), f->fsk->Nbits);
        /* Convert float samps to short */
        for(i=0; i<f->n_nom_modem_samples; i++){
            mod_out[i] = (short)(tx_float[i]*FSK_SCALE);
        }
    /* do me-fsk mod */
    } else if(FDV_MODE_ACTIVE( FREEDV_MODE_2400B, f->mode)){
        fmfsk_mod(f->fmfsk,tx_float,(uint8_t*)(f->tx_bits));
        /* Convert float samps to short */
        for(i=0; i<f->n_nom_modem_samples; i++){
            mod_out[i] = (short)(tx_float[i]*FMFSK_SCALE);
        }
    }
}


int freedv_tx_fsk_ldpc_bits_per_frame(struct freedv *f) { return f->ldpc->coded_bits_per_frame + sizeof(fsk_ldpc_uw); }

/* in a separate function so callable by other FSK Txs */
void freedv_tx_fsk_ldpc_framer(struct freedv *f, uint8_t frame[], uint8_t payload_data[]) {

    /* lets build up the frame to Tx ............. */

    /* insert UW */
    memcpy(frame, fsk_ldpc_uw, sizeof(fsk_ldpc_uw));
    /* insert data bits */
    memcpy(frame + sizeof(fsk_ldpc_uw), payload_data, f->bits_per_modem_frame);
    /* insert parity bits */
    encode(f->ldpc, frame + sizeof(fsk_ldpc_uw), frame + sizeof(fsk_ldpc_uw) + f->bits_per_modem_frame);  
}

/* FreeDV FSK_LDPC mode tx */
void freedv_tx_fsk_ldpc_data(struct freedv *f, COMP mod_out[]) {
    int bits_per_frame = freedv_tx_fsk_ldpc_bits_per_frame(f);
    uint8_t frame[bits_per_frame];
    int   i;
    
    assert(f->mode == FREEDV_MODE_FSK_LDPC);
   
    freedv_tx_fsk_ldpc_framer(f, frame, f->tx_payload_bits);    
    fsk_mod_c(f->fsk, mod_out, frame, bits_per_frame);

    /* Convert float samps to short */
    for(i=0; i<f->n_nom_modem_samples; i++) {
        mod_out[i].real *= FSK_SCALE;
        mod_out[i].imag *= FSK_SCALE;
    }
}


/* FreeDV FSK_LDPC mode rx */
int freedv_rx_fsk_ldpc_data(struct freedv *f, COMP demod_in[]) {
    int bits_per_frame = freedv_tx_fsk_ldpc_bits_per_frame(f);
    struct FSK *fsk = f->fsk;
    float rx_filt[fsk->mode*fsk->Nsym];
    float llrs[fsk->Nbits];
    int   rx_status = 0;
    
    /* demodulate to bit LLRs */
    
    fsk_demod_sd(fsk, rx_filt, demod_in);
    fsk_rx_filt_to_llrs(llrs, rx_filt, fsk->v_est, fsk->SNRest, fsk->mode, fsk->Nsym);
    f->nin = fsk_nin(f->fsk);
    f->snr_est = fsk->SNRest;

    /* OK we have fsk->Nbits new bits/symbols, place them at the end of the buffers */

    memmove(f->twoframes_llr,
            &f->twoframes_llr[fsk->Nbits],
            (2*bits_per_frame-fsk->Nbits)*sizeof(float));
    memcpy(&f->twoframes_llr[2*bits_per_frame-fsk->Nbits], llrs, fsk->Nbits*sizeof(float));
    memmove(f->twoframes_hard,
            &f->twoframes_hard[fsk->Nbits],
            2*bits_per_frame-fsk->Nbits);

    /* convert bit LLRs to hard decisions */
    for(int i=0; i<fsk->Nbits; i++) {
        if (llrs[i] < 0)
            f->twoframes_hard[2*bits_per_frame - fsk->Nbits + i] = 1;
        else
            f->twoframes_hard[2*bits_per_frame - fsk->Nbits + i] = 0;
    }

    /* used to track where UW (and latest frame) will be */
    f->fsk_ldpc_nbits += fsk->Nbits;
    f->fsk_ldpc_newbits += fsk->Nbits;

    if (f->fsk_ldpc_nbits > bits_per_frame) {
        // OK we have accumulated at least one new frame of bits, lets run
        // deframer state machine

        if (f->verbose)
            fprintf(stderr, "state: %d nbits: %d newbits: %d best_location: %d\n",
                    f->fsk_ldpc_state, f->fsk_ldpc_nbits, f->fsk_ldpc_newbits, f->fsk_ldpc_best_location);
        int errors = 0;
        int next_state = f->fsk_ldpc_state;
        switch(f->fsk_ldpc_state) {
        case 0: /* out of sync - look for UW */
            f->fsk_ldpc_best_location = 0;
            int best_errors = sizeof(fsk_ldpc_uw);
            for(int i=0; i<bits_per_frame; i++) {
                errors = 0;
                for(int u=0; u<sizeof(fsk_ldpc_uw); u++)
                    errors += f->twoframes_hard[i+u] ^ fsk_ldpc_uw[u];
                //fprintf(stderr, "  errors: %d %d %d\n", i, errors, best_errors);
                if (errors < best_errors) { best_errors = errors; f->fsk_ldpc_best_location = i; }
            }
            if (best_errors <= f->fsk_ldpc_thresh1) {
                if (f->verbose) fprintf(stderr, "  found UW!\n");
                next_state = 1; f->fsk_ldpc_baduw = 0;
            }
            break;
        case 1: /* in sync */
            /* work out where UW should be, this is tricky as we don't input exactly bits_per_frame, 
               but rather some multiple of fsk->Nbits */
            f->fsk_ldpc_best_location += bits_per_frame - f->fsk_ldpc_newbits;
            /* check UW still OK */
            for(int u=0; u<sizeof(fsk_ldpc_uw); u++)
                errors += f->twoframes_hard[f->fsk_ldpc_best_location+u] ^ fsk_ldpc_uw[u];
            if (f->verbose) fprintf(stderr, "%d errors: %d bad_uw: %d\n", f->fsk_ldpc_best_location, errors, f->fsk_ldpc_baduw);
            if (errors >= f->fsk_ldpc_thresh2) {
                f->fsk_ldpc_baduw++;
                if (f->fsk_ldpc_baduw == 3) {
                    //fprintf(stderr, "  lost UW!\n");
                    next_state = 0;
                }
            }
            else f->fsk_ldpc_baduw = 0;
            break;
        }
        f->fsk_ldpc_state = next_state;
        f->fsk_ldpc_nbits -= bits_per_frame;
        f->fsk_ldpc_newbits = 0;
        
        if (f->fsk_ldpc_state == 1) {
            int parityCheckCount;
            run_ldpc_decoder(f->ldpc, f->rx_payload_bits,
                             &f->twoframes_llr[f->fsk_ldpc_best_location+sizeof(fsk_ldpc_uw)],
                             &parityCheckCount);
            if (f->verbose) fprintf(stderr, "parityCheckCount: %d\n",parityCheckCount);
            rx_status |= RX_BITS;
            if (parityCheckCount != f->ldpc->NumberParityBits)
                rx_status |= RX_BIT_ERRORS;

            if (f->test_frames) {
                /* regenerate tx test frame */
                uint8_t tx_frame[bits_per_frame];
                memcpy(tx_frame, fsk_ldpc_uw, sizeof(fsk_ldpc_uw));
                ofdm_generate_payload_data_bits(tx_frame + sizeof(fsk_ldpc_uw), f->bits_per_modem_frame);
                encode(f->ldpc, tx_frame + sizeof(fsk_ldpc_uw), tx_frame + sizeof(fsk_ldpc_uw) + f->bits_per_modem_frame);  
                
                /* count uncoded (raw) errors across UW, payload bits, parity bits */
                int Nerrs_raw = count_errors(tx_frame, f->twoframes_hard + f->fsk_ldpc_best_location, bits_per_frame);
                f->total_bit_errors += Nerrs_raw;
                f->total_bits += bits_per_frame;

                /* count coded errors across just payload bits */
                int Nerrs_coded = count_errors(tx_frame + sizeof(fsk_ldpc_uw), f->rx_payload_bits, f->bits_per_modem_frame);
                f->total_bit_errors_coded += Nerrs_coded;
                f->total_bits_coded += f->bits_per_modem_frame;
            }
        }
    }

    if (f->fsk_ldpc_state == 1) rx_status |= RX_SYNC;
    
    return rx_status;
}


int freedv_comprx_fsk(struct freedv *f, COMP demod_in[]) {
    /* Varicode and protocol bits */
    uint8_t vc_bits[2];
    uint8_t proto_bits[3];
    short vc_bit;
    int i;
    int n_ascii;
    char ascii_out;
    int rx_status = 0;
    
    if(FDV_MODE_ACTIVE( FREEDV_MODE_2400A, f->mode) || FDV_MODE_ACTIVE( FREEDV_MODE_800XA, f->mode)){        
	fsk_demod(f->fsk,(uint8_t*)f->tx_bits,demod_in);
        f->nin = fsk_nin(f->fsk);
        float EbNodB = f->fsk->stats->snr_est;           /* fsk demod actually estimates Eb/No     */
        f->snr_est = EbNodB + 10.0*log10f(800.0/3000.0); /* so convert to SNR Rb=800, noise B=3000 */
    } else{      
        /* 2400B needs real input samples */
        int n = fmfsk_nin(f->fmfsk);
        float demod_in_float[n];
        for(i=0; i<n; i++) {
            demod_in_float[i] = demod_in[i].real;
        }
        fmfsk_demod(f->fmfsk,(uint8_t*)f->tx_bits,demod_in_float);
        f->nin = fmfsk_nin(f->fmfsk);
    }
    
    rx_status = fvhff_deframe_bits(f->deframer,f->rx_payload_bits,proto_bits,vc_bits,(uint8_t*)f->tx_bits);
    if((rx_status & RX_SYNC) && (rx_status & RX_BITS)){
        /* Decode varicode text */
        for(i=0; i<2; i++){
            /* Note: deframe_bits spits out bits in uint8_ts while varicode_decode expects shorts */
            vc_bit = vc_bits[i];
            n_ascii = varicode_decode(&f->varicode_dec_states, &ascii_out, &vc_bit, 1, 1);
            if (n_ascii && (f->freedv_put_next_rx_char != NULL)) {
                (*f->freedv_put_next_rx_char)(f->callback_state, ascii_out);
            }
        }
        /* Pass proto bits on down if callback is present */
        if( f->freedv_put_next_proto != NULL){
            (*f->freedv_put_next_proto)(f->proto_callback_state,(char*)proto_bits);
        }
    } 
    f->sync = f->deframer->state;
    f->stats.sync = f->deframer->state;

    return rx_status;
}


int freedv_floatrx(struct freedv *f, short speech_out[], float demod_in[]) {
    assert(f != NULL);
    int  i;
    int nin = freedv_nin(f);    
    
    assert(nin <= f->n_max_modem_samples);
    
    COMP rx_fdm[f->n_max_modem_samples];
    for(i=0; i<nin; i++) {
        rx_fdm[i].real = demod_in[i];
        rx_fdm[i].imag = 0;
    }

    return freedv_comprx(f, speech_out, rx_fdm);
}
