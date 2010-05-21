/* Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2009 Xiph.Org Foundation
   Copyright (c) 2008-2009 Gregory Maxwell 
   Written by Jean-Marc Valin and Gregory Maxwell */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include "bands.h"
#include "modes.h"
#include "vq.h"
#include "cwrs.h"
#include "stack_alloc.h"
#include "os_support.h"
#include "mathops.h"
#include "rate.h"


#ifdef FIXED_POINT
/* Compute the amplitude (sqrt energy) in each of the bands */
void compute_band_energies(const CELTMode *m, const celt_sig *X, celt_ener *bank, int _C, int M)
{
   int i, c, N;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   N = M*m->eBands[m->nbEBands+1];
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word32 maxval=0;
         celt_word32 sum = 0;
         
         j=M*eBands[i]; do {
            maxval = MAX32(maxval, X[j+c*N]);
            maxval = MAX32(maxval, -X[j+c*N]);
         } while (++j<M*eBands[i+1]);
         
         if (maxval > 0)
         {
            int shift = celt_ilog2(maxval)-10;
            j=M*eBands[i]; do {
               sum = MAC16_16(sum, EXTRACT16(VSHR32(X[j+c*N],shift)),
                                   EXTRACT16(VSHR32(X[j+c*N],shift)));
            } while (++j<M*eBands[i+1]);
            /* We're adding one here to make damn sure we never end up with a pitch vector that's
               larger than unity norm */
            bank[i+c*m->nbEBands] = EPSILON+VSHR32(EXTEND32(celt_sqrt(sum)),-shift);
         } else {
            bank[i+c*m->nbEBands] = EPSILON;
         }
         /*printf ("%f ", bank[i+c*m->nbEBands]);*/
      }
   }
   /*printf ("\n");*/
}

/* Normalise each band such that the energy is one. */
void normalise_bands(const CELTMode *m, const celt_sig * restrict freq, celt_norm * restrict X, const celt_ener *bank, int _C, int M)
{
   int i, c, N;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   N = M*m->eBands[m->nbEBands+1];
   for (c=0;c<C;c++)
   {
      i=0; do {
         celt_word16 g;
         int j,shift;
         celt_word16 E;
         shift = celt_zlog2(bank[i+c*m->nbEBands])-13;
         E = VSHR32(bank[i+c*m->nbEBands], shift);
         g = EXTRACT16(celt_rcp(SHL32(E,3)));
         j=M*eBands[i]; do {
            X[j+c*N] = MULT16_16_Q15(VSHR32(freq[j+c*N],shift-1),g);
         } while (++j<M*eBands[i+1]);
      } while (++i<m->nbEBands);
   }
}

#else /* FIXED_POINT */
/* Compute the amplitude (sqrt energy) in each of the bands */
void compute_band_energies(const CELTMode *m, const celt_sig *X, celt_ener *bank, int _C, int M)
{
   int i, c, N;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   N = M*m->eBands[m->nbEBands+1];
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word32 sum = 1e-10;
         for (j=M*eBands[i];j<M*eBands[i+1];j++)
            sum += X[j+c*N]*X[j+c*N];
         bank[i+c*m->nbEBands] = sqrt(sum);
         /*printf ("%f ", bank[i+c*m->nbEBands]);*/
      }
   }
   /*printf ("\n");*/
}

/* Normalise each band such that the energy is one. */
void normalise_bands(const CELTMode *m, const celt_sig * restrict freq, celt_norm * restrict X, const celt_ener *bank, int _C, int M)
{
   int i, c, N;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   N = M*m->eBands[m->nbEBands+1];
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         celt_word16 g = 1.f/(1e-10f+bank[i+c*m->nbEBands]);
         for (j=M*eBands[i];j<M*eBands[i+1];j++)
            X[j+c*N] = freq[j+c*N]*g;
      }
   }
}

#endif /* FIXED_POINT */

void renormalise_bands(const CELTMode *m, celt_norm * restrict X, int _C, int M)
{
   int i, c;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   for (c=0;c<C;c++)
   {
      i=0; do {
         renormalise_vector(X+M*eBands[i]+c*M*eBands[m->nbEBands+1], Q15ONE, M*eBands[i+1]-M*eBands[i], 1);
      } while (++i<m->nbEBands);
   }
}

/* De-normalise the energy to produce the synthesis from the unit-energy bands */
void denormalise_bands(const CELTMode *m, const celt_norm * restrict X, celt_sig * restrict freq, const celt_ener *bank, int _C, int M)
{
   int i, c, N;
   const celt_int16 *eBands = m->eBands;
   const int C = CHANNELS(_C);
   N = M*m->eBands[m->nbEBands+1];
   if (C>2)
      celt_fatal("denormalise_bands() not implemented for >2 channels");
   for (c=0;c<C;c++)
   {
      celt_sig * restrict f;
      const celt_norm * restrict x;
      f = freq+c*N;
      x = X+c*N;
      for (i=0;i<m->nbEBands;i++)
      {
         int j, end;
         celt_word32 g = SHR32(bank[i+c*m->nbEBands],1);
         j=M*eBands[i];
         end = M*eBands[i+1];
         do {
            *f++ = SHL32(MULT16_32_Q15(*x, g),2);
            x++;
         } while (++j<end);
      }
      for (i=M*eBands[m->nbEBands];i<M*eBands[m->nbEBands+1];i++)
         *f++ = 0;
   }
}

int compute_pitch_gain(const CELTMode *m, const celt_sig *X, const celt_sig *P, int norm_rate, int *gain_id, int _C, celt_word16 *gain_prod, int M)
{
   int j, c;
   celt_word16 g;
   celt_word16 delta;
   const int C = CHANNELS(_C);
   celt_word32 Sxy=0, Sxx=0, Syy=0;
   int len = M*m->pitchEnd;
   int N = M*m->eBands[m->nbEBands+1];
#ifdef FIXED_POINT
   int shift = 0;
   celt_word32 maxabs=0;

   for (c=0;c<C;c++)
   {
      for (j=0;j<len;j++)
      {
         maxabs = MAX32(maxabs, ABS32(X[j+c*N]));
         maxabs = MAX32(maxabs, ABS32(P[j+c*N]));
      }
   }
   shift = celt_ilog2(maxabs)-12;
   if (shift<0)
      shift = 0;
#endif
   delta = PDIV32_16(Q15ONE, len);
   for (c=0;c<C;c++)
   {
      celt_word16 gg = Q15ONE;
      for (j=0;j<len;j++)
      {
         celt_word16 Xj, Pj;
         Xj = EXTRACT16(SHR32(X[j+c*N], shift));
         Pj = MULT16_16_P15(gg,EXTRACT16(SHR32(P[j+c*N], shift)));
         Sxy = MAC16_16(Sxy, Xj, Pj);
         Sxx = MAC16_16(Sxx, Pj, Pj);
         Syy = MAC16_16(Syy, Xj, Xj);
         gg = SUB16(gg, delta);
      }
   }
#ifdef FIXED_POINT
   {
      celt_word32 num, den;
      celt_word16 fact;
      fact = MULT16_16(QCONST16(.04f, 14), norm_rate);
      if (fact < QCONST16(1.f, 14))
         fact = QCONST16(1.f, 14);
      num = Sxy;
      den = EPSILON+Sxx+MULT16_32_Q15(QCONST16(.03f,15),Syy);
      shift = celt_zlog2(Sxy)-16;
      if (shift < 0)
         shift = 0;
      if (Sxy < MULT16_32_Q15(fact, MULT16_16(celt_sqrt(EPSILON+Sxx),celt_sqrt(EPSILON+Syy))))
         g = 0;
      else
         g = DIV32(SHL32(SHR32(num,shift),14),ADD32(EPSILON,SHR32(den,shift)));

      /* This MUST round down so that we don't over-estimate the gain */
      *gain_id = EXTRACT16(SHR32(MULT16_16(20,(g-QCONST16(.5f,14))),14));
   }
#else
   {
      float fact = .04f*norm_rate;
      if (fact < 1)
         fact = 1;
      g = Sxy/(.1f+Sxx+.03f*Syy);
      if (Sxy < .5f*fact*celt_sqrt(1+Sxx*Syy))
         g = 0;
      /* This MUST round down so that we don't over-estimate the gain */
      *gain_id = floor(20*(g-.5f));
   }
#endif
   /* This prevents the pitch gain from being above 1.0 for too long by bounding the 
      maximum error amplification factor to 2.0 */
   g = ADD16(QCONST16(.5f,14), MULT16_16_16(QCONST16(.05f,14),*gain_id));
   *gain_prod = MAX16(QCONST32(1.f, 13), MULT16_16_Q14(*gain_prod,g));
   if (*gain_prod>QCONST32(2.f, 13))
   {
      *gain_id=9;
      *gain_prod = QCONST32(2.f, 13);
   }

   if (*gain_id < 0)
   {
      *gain_id = 0;
      return 0;
   } else {
      if (*gain_id > 15)
         *gain_id = 15;
      return 1;
   }
}

void apply_pitch(const CELTMode *m, celt_sig *X, const celt_sig *P, int gain_id, int pred, int _C, int M)
{
   int j, c, N;
   celt_word16 gain;
   celt_word16 delta;
   const int C = CHANNELS(_C);
   int len = M*m->pitchEnd;

   N = M*m->eBands[m->nbEBands+1];
   gain = ADD16(QCONST16(.5f,14), MULT16_16_16(QCONST16(.05f,14),gain_id));
   delta = PDIV32_16(gain, len);
   if (pred)
      gain = -gain;
   else
      delta = -delta;
   for (c=0;c<C;c++)
   {
      celt_word16 gg = gain;
      for (j=0;j<len;j++)
      {
         X[j+c*N] += SHL32(MULT16_32_Q15(gg,P[j+c*N]),1);
         gg = ADD16(gg, delta);
      }
   }
}

static void stereo_band_mix(const CELTMode *m, celt_norm *X, celt_norm *Y, const celt_ener *bank, int stereo_mode, int bandID, int dir, int N)
{
   int i = bandID;
   const celt_int16 *eBands = m->eBands;
   int j;
   celt_word16 a1, a2;
   if (stereo_mode==0)
   {
      /* Do mid-side when not doing intensity stereo */
      a1 = QCONST16(.70711f,14);
      a2 = dir*QCONST16(.70711f,14);
   } else {
      celt_word16 left, right;
      celt_word16 norm;
#ifdef FIXED_POINT
      int shift = celt_zlog2(MAX32(bank[i], bank[i+m->nbEBands]))-13;
#endif
      left = VSHR32(bank[i],shift);
      right = VSHR32(bank[i+m->nbEBands],shift);
      norm = EPSILON + celt_sqrt(EPSILON+MULT16_16(left,left)+MULT16_16(right,right));
      a1 = DIV32_16(SHL32(EXTEND32(left),14),norm);
      a2 = dir*DIV32_16(SHL32(EXTEND32(right),14),norm);
   }
   for (j=0;j<N;j++)
   {
      celt_norm r, l;
      l = X[j];
      r = Y[j];
      X[j] = MULT16_16_Q14(a1,l) + MULT16_16_Q14(a2,r);
      Y[j] = MULT16_16_Q14(a1,r) - MULT16_16_Q14(a2,l);
   }
}


int folding_decision(const CELTMode *m, celt_norm *X, celt_word16 *average, int *last_decision, int _C, int M)
{
   int i, c, N0;
   int NR=0;
   celt_word32 ratio = EPSILON;
   const int C = CHANNELS(_C);
   const celt_int16 * restrict eBands = m->eBands;
   
   N0 = M*m->eBands[m->nbEBands+1];

   for (c=0;c<C;c++)
   {
   for (i=0;i<m->nbEBands;i++)
   {
      int j, N;
      int max_i=0;
      celt_word16 max_val=EPSILON;
      celt_word32 floor_ener=EPSILON;
      celt_norm * restrict x = X+M*eBands[i]+c*N0;
      N = M*eBands[i+1]-M*eBands[i];
      for (j=0;j<N;j++)
      {
         if (ABS16(x[j])>max_val)
         {
            max_val = ABS16(x[j]);
            max_i = j;
         }
      }
#if 0
      for (j=0;j<N;j++)
      {
         if (abs(j-max_i)>2)
            floor_ener += x[j]*x[j];
      }
#else
      floor_ener = QCONST32(1.,28)-MULT16_16(max_val,max_val);
      if (max_i < N-1)
         floor_ener -= MULT16_16(x[(max_i+1)], x[(max_i+1)]);
      if (max_i < N-2)
         floor_ener -= MULT16_16(x[(max_i+2)], x[(max_i+2)]);
      if (max_i > 0)
         floor_ener -= MULT16_16(x[(max_i-1)], x[(max_i-1)]);
      if (max_i > 1)
         floor_ener -= MULT16_16(x[(max_i-2)], x[(max_i-2)]);
      floor_ener = MAX32(floor_ener, EPSILON);
#endif
      if (N>7)
      {
         celt_word16 r;
         celt_word16 den = celt_sqrt(floor_ener);
         den = MAX32(QCONST16(.02f, 15), den);
         r = DIV32_16(SHL32(EXTEND32(max_val),8),den);
         ratio = ADD32(ratio, EXTEND32(r));
         NR++;
      }
   }
   }
   if (NR>0)
      ratio = DIV32_16(ratio, NR);
   ratio = ADD32(HALF32(ratio), HALF32(*average));
   if (!*last_decision)
   {
      *last_decision = (ratio < QCONST16(1.8f,8));
   } else {
      *last_decision = (ratio < QCONST16(3.f,8));
   }
   *average = EXTRACT16(ratio);
   return *last_decision;
}

/* This function is responsible for encoding and decoding a band for both
   the mono and stereo case. Even in the mono case, it can split the band
   in two and transmit the energy difference with the two half-bands. It
   can be called recursively so bands can end up being split in 8 parts. */
static void quant_band(int encode, const CELTMode *m, int i, celt_norm *X, celt_norm *Y, int N, int b, int spread, celt_norm *lowband, int resynth, ec_enc *ec, celt_int32 *remaining_bits, int LM, celt_norm *lowband_out, const celt_ener *bandE)
{
   int q;
   int curr_bits;
   int stereo, split;
   int imid=0, iside=0;
   int N0=N;

   split = stereo = Y != NULL;

   /* If we need more than 32 bits, try splitting the band in two. */
   if (!stereo && LM != -1 && !fits_in32(N, get_pulses(bits2pulses(m, m->bits[LM][i], N, b))))
   {
      if (LM>0 || (N&1)==0)
      {
         N >>= 1;
         Y = X+N;
         split = 1;
         LM -= 1;
      }
   }

   if (split)
   {
      int qb;
      int itheta;
      int mbits, sbits, delta;
      int qalloc;
      celt_word16 mid, side;
      if (N>1)
         qb = (b-2*(N-1)*(QTHETA_OFFSET-m->logN[i]-(LM<<BITRES)))/(32*(N-1));
      else
         qb = b-2;
      if (qb > (b>>BITRES)-1)
         qb = (b>>BITRES)-1;
      if (qb<0)
         qb = 0;
      if (qb>14)
         qb = 14;

      if (encode)
      {
         if (stereo)
            stereo_band_mix(m, X, Y, bandE, qb==0, i, 1, N);

         mid = renormalise_vector(X, Q15ONE, N, 1);
         side = renormalise_vector(Y, Q15ONE, N, 1);

         /* 0.63662 = 2/pi */
#ifdef FIXED_POINT
         itheta = MULT16_16_Q15(QCONST16(0.63662f,15),celt_atan2p(side, mid));
#else
         itheta = floor(.5f+16384*0.63662f*atan2(side,mid));
#endif
      }

      qalloc = log2_frac((1<<qb)+1,BITRES);
      if (qb==0)
      {
         itheta=0;
      } else {
         int shift;
         int fs=1, ft;
         shift = 14-qb;
         ft = ((1<<qb>>1)+1)*((1<<qb>>1)+1);

         /* Entropy coding of the angle. We use a uniform pdf for the
            first stereo split but a triangular one for the rest. */
         if (encode)
            itheta = (itheta+(1<<shift>>1))>>shift;
         if (stereo || qb>9)
         {
            if (encode)
               ec_enc_uint(ec, itheta, (1<<qb)+1);
            else
               itheta = ec_dec_uint((ec_dec*)ec, (1<<qb)+1);
         } else {
            if (encode)
            {
               int j;
               int fl=0;
               j=0;
               while(1)
               {
                  if (j==itheta)
                     break;
                  fl+=fs;
                  if (j<(1<<qb>>1))
                     fs++;
                  else
                     fs--;
                  j++;
               }
               ec_encode(ec, fl, fl+fs, ft);
            } else {
               int fl=0;
               int j, fm;
               fm = ec_decode((ec_dec*)ec, ft);
               j=0;
               while (1)
               {
                  if (fm < fl+fs)
                     break;
                  fl+=fs;
                  if (j<(1<<qb>>1))
                     fs++;
                  else
                     fs--;
                  j++;
               }
               itheta = j;
               ec_dec_update((ec_dec*)ec, fl, fl+fs, ft);
            }
            qalloc = log2_frac(ft,BITRES) - log2_frac(fs,BITRES) + 1;
         }
         itheta <<= shift;
      }

      if (itheta == 0)
      {
         imid = 32767;
         iside = 0;
         delta = -10000;
      } else if (itheta == 16384)
      {
         imid = 0;
         iside = 32767;
         delta = 10000;
      } else {
         imid = bitexact_cos(itheta);
         iside = bitexact_cos(16384-itheta);
         delta = (N-1)*(log2_frac(iside,BITRES+2)-log2_frac(imid,BITRES+2))>>2;
      }

      /* This is a special case for N=2 that only works for stereo and takes
         advantage of the fact that mid and side are orthogonal to encode
         the side with just one bit. */
      if (N==2 && stereo)
      {
         int c, c2;
         int sign=1;
         celt_norm v[2], w[2];
         celt_norm *x2, *y2;
         mbits = b-qalloc;
         sbits = 0;
         if (itheta != 0 && itheta != 16384)
            sbits = 1<<BITRES;
         mbits -= sbits;
         c = itheta > 8192 ? 1 : 0;
         *remaining_bits -= qalloc+sbits;

         x2 = X;
         y2 = Y;
         if (encode)
         {
            c2 = 1-c;

            if (c==0)
            {
               v[0] = x2[0];
               v[1] = x2[1];
               w[0] = y2[0];
               w[1] = y2[1];
            } else {
               v[0] = y2[0];
               v[1] = y2[1];
               w[0] = x2[0];
               w[1] = x2[1];
            }
         }
         quant_band(encode, m, i, v, NULL, N, mbits, spread, lowband, resynth, ec, remaining_bits, LM, NULL, NULL);
         if (sbits)
         {
            if (encode)
            {
               /* Here we only need to encode a sign for the side */
               if (v[0]*w[1] - v[1]*w[0] > 0)
                  sign = 1;
               else
                  sign = -1;
               ec_enc_bits(ec, sign==1, 1);
            } else {
               sign = 2*ec_dec_bits((ec_dec*)ec, 1)-1;
            }
         } else {
            sign = 1;
         }
         w[0] = -sign*v[1];
         w[1] = sign*v[0];
         if (c==0)
         {
            x2[0] = v[0];
            x2[1] = v[1];
            y2[0] = w[0];
            y2[1] = w[1];
         } else {
            x2[0] = w[0];
            x2[1] = w[1];
            y2[0] = v[0];
            y2[1] = v[1];
         }
      } else
      {
         /* "Normal" split code */
         mbits = (b-qalloc/2-delta)/2;
         if (mbits > b-qalloc)
            mbits = b-qalloc;
         if (mbits<0)
            mbits=0;
         sbits = b-qalloc-mbits;
         *remaining_bits -= qalloc;
         quant_band(encode, m, i, X, NULL, N, mbits, spread, lowband, resynth, ec, remaining_bits, LM, NULL, NULL);
         if (stereo)
            quant_band(encode, m, i, Y, NULL, N, sbits, spread, NULL, resynth, ec, remaining_bits, LM, NULL, NULL);
         else
            quant_band(encode, m, i, Y, NULL, N, sbits, spread, lowband ? lowband+N : NULL, resynth, ec, remaining_bits, LM, NULL, NULL);
      }

   } else {
      /* This is the basic no-split case */
      q = bits2pulses(m, m->bits[LM][i], N, b);
      curr_bits = pulses2bits(m->bits[LM][i], N, q);
      *remaining_bits -= curr_bits;

      /* Ensures we can never bust the budget */
      while (*remaining_bits < 0 && q > 0)
      {
         *remaining_bits += curr_bits;
         q--;
         curr_bits = pulses2bits(m->bits[LM][i], N, q);
         *remaining_bits -= curr_bits;
      }

      /* Making sure we will *never* need more than 32 bits for the PVQ */
      while (!fits_in32(N, get_pulses(q)))
         q--;

      if (encode)
         alg_quant(X, N, q, spread, lowband, resynth, ec);
      else
         alg_unquant(X, N, q, spread, lowband, (ec_dec*)ec);
   }

   if (resynth && lowband_out)
   {
      int j;
      celt_word16 n;
      n = celt_sqrt(SHL32(EXTEND32(N0),22));
      for (j=0;j<N0;j++)
         lowband_out[j] = MULT16_16_Q15(n,X[j]);
   }

   if (split && resynth)
   {
      int j;
      celt_word16 mid, side;
#ifdef FIXED_POINT
      mid = imid;
      side = iside;
#else
      mid = (1.f/32768)*imid;
      side = (1.f/32768)*iside;
#endif
      for (j=0;j<N;j++)
         X[j] = MULT16_16_Q15(X[j], mid);
      for (j=0;j<N;j++)
         Y[j] = MULT16_16_Q15(Y[j], side);

   }
}

void quant_all_bands(int encode, const CELTMode *m, int start, celt_norm *_X, celt_norm *_Y, const celt_ener *bandE, int *pulses, int shortBlocks, int fold, int resynth, int total_bits, ec_enc *ec, int LM)
{
   int i, remaining_bits, balance;
   const celt_int16 * restrict eBands = m->eBands;
   celt_norm * restrict norm;
   VARDECL(celt_norm, _norm);
   int B;
   int M;
   int spread;
   SAVE_STACK;

   M = 1<<LM;
   B = shortBlocks ? M : 1;
   spread = fold ? B : 0;
   ALLOC(_norm, M*eBands[m->nbEBands+1], celt_norm);
   norm = _norm;
   /* Just in case the first bands attempts to fold -- not that rare for stereo */
   for (i=0;i<M;i++)
      norm[i] = 0;

   balance = 0;
   for (i=start;i<m->nbEBands;i++)
   {
      int tell;
      int b;
      int N;
      int curr_balance;
      celt_norm * restrict X, * restrict Y;
      
      X = _X+M*eBands[i];
      if (_Y!=NULL)
         Y = _Y+M*eBands[i];
      else
         Y = NULL;
      N = M*eBands[i+1]-M*eBands[i];
      if (encode)
         tell = ec_enc_tell(ec, BITRES);
      else
         tell = ec_dec_tell((ec_dec*)ec, BITRES);

      if (i != start)
         balance -= tell;
      remaining_bits = (total_bits<<BITRES)-tell-1;
      curr_balance = (m->nbEBands-i);
      if (curr_balance > 3)
         curr_balance = 3;
      curr_balance = balance / curr_balance;
      b = IMIN(remaining_bits+1,pulses[i]+curr_balance);
      if (b<0)
         b = 0;

      quant_band(encode, m, i, X, Y, N, b, spread, norm+M*eBands[start], resynth, ec, &remaining_bits, LM, norm+M*eBands[i], bandE);

      balance += pulses[i] + tell;

      if (resynth && _Y != NULL)
      {
         stereo_band_mix(m, X, Y, bandE, 0, i, -1, N);
         renormalise_vector(X, Q15ONE, N, 1);
         renormalise_vector(Y, Q15ONE, N, 1);
      }
   }
   RESTORE_STACK;
}

