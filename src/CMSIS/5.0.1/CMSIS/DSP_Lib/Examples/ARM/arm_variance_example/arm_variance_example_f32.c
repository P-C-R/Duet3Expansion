/* ----------------------------------------------------------------------
* Copyright (C) 2010-2012 ARM Limited. All rights reserved.
*
* $Date:         17. January 2013
* $Revision:     V1.4.0
*
* Project:       CMSIS DSP Library
* Title:         arm_variance_example_f32.c
*
* Description:   Example code demonstrating variance calculation of input sequence.
*
* Target Processor: Cortex-M4/Cortex-M3
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*   - Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   - Redistributions in binary form must reproduce the above copyright
*     notice, this list of conditions and the following disclaimer in
*     the documentation and/or other materials provided with the
*     distribution.
*   - Neither the name of ARM LIMITED nor the names of its contributors
*     may be used to endorse or promote products derived from this
*     software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
* -------------------------------------------------------------------- */

/**
 * @ingroup groupExamples
 */

/**
 * @defgroup VarianceExample Variance Example
 *
 * \par Description:
 * \par
 * Demonstrates the use of Basic Math and Support Functions to calculate the variance of an
 * input sequence with N samples. Uniformly distributed white noise is taken as input.
 *
 * \par Algorithm:
 * \par
 * The variance of a sequence is the mean of the squared deviation of the sequence from its mean.
 * \par
 * This is denoted by the following equation:
 * <pre> variance = ((x[0] - x') * (x[0] - x') + (x[1] - x') * (x[1] - x') + ... + * (x[n-1] - x') * (x[n-1] - x')) / (N-1)</pre>
 * where, <code>x[n]</code> is the input sequence, <code>N</code> is the number of input samples, and
 * <code>x'</code> is the mean value of the input sequence, <code>x[n]</code>.
 * \par
 * The mean value <code>x'</code> is defined as:
 * <pre> x' = (x[0] + x[1] + ... + x[n-1]) / N</pre>
 *
 * \par Block Diagram:
 * \par
 * \image html Variance.gif
 *
 *
 * \par Variables Description:
 * \par
 * \li \c testInput_f32 points to the input data
 * \li \c wire1, \c wir2, \c wire3 temporary buffers
 * \li \c blockSize number of samples processed at a time
 * \li \c refVarianceOut reference variance value
 *
 * \par CMSIS DSP Software Library Functions Used:
 * \par
 * - arm_dot_prod_f32()
 * - arm_mult_f32()
 * - arm_sub_f32()
 * - arm_fill_f32()
 * - arm_copy_f32()
 *
 * <b> Refer  </b>
 * \link arm_variance_example_f32.c \endlink
 *
 */


/** \example arm_variance_example_f32.c
  */
#include <math.h>

#include "../../../../../../../CMSIS/5.0.1/CMSIS/Include/arm_math.h"

/* ----------------------------------------------------------------------
* Defines each of the tests performed
* ------------------------------------------------------------------- */
#define MAX_BLOCKSIZE   32
#define DELTA           (0.000001f)


/* ----------------------------------------------------------------------
* Declare I/O buffers
* ------------------------------------------------------------------- */
float32_t wire1[MAX_BLOCKSIZE];
float32_t wire2[MAX_BLOCKSIZE];
float32_t wire3[MAX_BLOCKSIZE];

/* ----------------------------------------------------------------------
* Test input data for Floating point Variance example for 32-blockSize
* Generated by the MATLAB randn() function
* ------------------------------------------------------------------- */

float32_t testInput_f32[32] =
{
  -0.432564811528221,  -1.665584378238097,   0.125332306474831,   0.287676420358549,
  -1.146471350681464,   1.190915465642999,   1.189164201652103,  -0.037633276593318,
   0.327292361408654,   0.174639142820925,  -0.186708577681439,   0.725790548293303,
  -0.588316543014189,   2.183185818197101,  -0.136395883086596,   0.113931313520810,
   1.066768211359189,   0.059281460523605,  -0.095648405483669,  -0.832349463650022,
   0.294410816392640,  -1.336181857937804,   0.714324551818952,   1.623562064446271,
  -0.691775701702287,   0.857996672828263,   1.254001421602532,  -1.593729576447477,
  -1.440964431901020,   0.571147623658178,  -0.399885577715363,   0.689997375464345

};

/* ----------------------------------------------------------------------
* Declare Global variables
* ------------------------------------------------------------------- */
uint32_t blockSize = 32;
float32_t  refVarianceOut = 0.903941793931839;

/* ----------------------------------------------------------------------
* Variance calculation test
* ------------------------------------------------------------------- */

int32_t main(void)
{
  arm_status status;
  float32_t mean, oneByBlockSize;
  float32_t variance;
  float32_t diff;

  status = ARM_MATH_SUCCESS;

  /* Calculation of mean value of input */

  /* x' = 1/blockSize * (x(0)* 1 + x(1) * 1 + ... + x(n-1) * 1) */

  /* Fill wire1 buffer with 1.0 value */
  arm_fill_f32(1.0,  wire1, blockSize);

  /* Calculate the dot product of wire1 and wire2 */
  /* (x(0)* 1 + x(1) * 1 + ...+ x(n-1) * 1) */
  arm_dot_prod_f32(testInput_f32, wire1, blockSize, &mean);

  /* Calculation of 1/blockSize */
  oneByBlockSize = 1.0 / (blockSize);

  /* 1/blockSize * (x(0)* 1 + x(1) * 1 + ... + x(n-1) * 1)  */
  arm_mult_f32(&mean, &oneByBlockSize, &mean, 1);


  /* Calculation of variance value of input */

  /* (1/blockSize) * (x(0) - x') * (x(0) - x') + (x(1) - x') * (x(1) - x') + ... + (x(n-1) - x') * (x(n-1) - x') */

  /* Fill wire2 with mean value x' */
  arm_fill_f32(mean,  wire2, blockSize);

  /* wire3 contains (x-x') */
  arm_sub_f32(testInput_f32, wire2, wire3, blockSize);

  /* wire2 contains (x-x') */
  arm_copy_f32(wire3, wire2, blockSize);

  /* (x(0) - x') * (x(0) - x') + (x(1) - x') * (x(1) - x') + ... + (x(n-1) - x') * (x(n-1) - x') */
  arm_dot_prod_f32(wire2, wire3, blockSize, &variance);

    /* Calculation of 1/blockSize */
  oneByBlockSize = 1.0 / (blockSize - 1);

  /* Calculation of variance */
  arm_mult_f32(&variance, &oneByBlockSize, &variance, 1);

  /* absolute value of difference between ref and test */
  diff = fabsf(refVarianceOut - variance);

  /* Comparison of variance value with reference */
  if (diff > DELTA)
  {
    status = ARM_MATH_TEST_FAILURE;
  }

  if ( status != ARM_MATH_SUCCESS)
  {
    while (1);
  }

  while (1);                             /* main function does not return */
}

 /** \endlink */
