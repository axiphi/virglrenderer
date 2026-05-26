/**************************************************************************
 *
 * Copyright (C) 2026 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <check.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tgsi/tgsi_text.h"
#include "vrend/vrend_shader.h"

static bool
shader_contains(const struct vrend_strarray *shader, const char *needle)
{
   for (int i = 0; i < shader->num_strings; i++) {
      if (strstr(shader->strings[i].buf, needle))
         return true;
   }

   return false;
}

static void
convert_tgsi(const char *tgsi, struct vrend_strarray *shader)
{
   struct tgsi_token tokens[1024];
   struct vrend_shader_cfg cfg = {
      .glsl_version = 330,
      .max_draw_buffers = 1,
      .max_shader_patch_varyings = 32,
   };
   struct vrend_shader_key key = { 0 };
   struct vrend_shader_info sinfo = { 0 };
   struct vrend_variable_shader_info var_sinfo = { 0 };

   ck_assert_msg(tgsi_text_translate(tgsi, tokens, sizeof(tokens) / sizeof(tokens[0])),
                 "failed to translate TGSI text");
   ck_assert_msg(strarray_alloc(shader, SHADER_MAX_STRINGS),
                 "failed to allocate shader string array");

   if (!vrend_convert_shader(NULL, &cfg, tokens, 0, &key, &sinfo, &var_sinfo, shader)) {
      strarray_free(shader, true);
      ck_abort_msg("failed to convert TGSI to GLSL");
   }
}

START_TEST(face_sources_are_vectors_for_compare_and_cmov)
{
   static const char tgsi[] =
      "FRAG\n"
      "DCL IN[0], FACE\n"
      "DCL OUT[0], COLOR\n"
      "DCL TEMP[0..2]\n"
      "IMM[0] FLT32 {0x00000000, 0x3f800000, 0xbf800000, 0x00000000}\n"
      "  0: MOV TEMP[0], IN[0]\n"
      "  1: FSGE TEMP[1], IN[0], IMM[0].xxxx\n"
      "  2: CMP TEMP[2], IN[0], TEMP[1], TEMP[0]\n"
      "  3: UCMP OUT[0], IN[0], TEMP[2], TEMP[1]\n"
      "  4: END\n";
   struct vrend_strarray shader;

   convert_tgsi(tgsi, &shader);

   ck_assert_msg(shader_contains(&shader, "vec4((gl_FrontFacing ? 1.0 : -1.0))"),
                 "FACE source was not emitted as a vector expression");
   ck_assert_msg(shader_contains(&shader, "notEqual(floatBitsToUint(vec4("),
                 "UCMP condition was not vectorized");

   ck_assert_msg(!shader_contains(&shader, "(((gl_FrontFacing ? 1.0 : -1.0)))"),
                 "FACE source was emitted as a scalar expression");
   ck_assert_msg(!shader_contains(&shader, "greaterThanEqual((gl_FrontFacing ? 1.0 : -1.0)"),
                 "CMP used a scalar FACE condition");
   ck_assert_msg(!shader_contains(&shader, "floatBitsToUint((gl_FrontFacing ? 1.0 : -1.0)"),
                 "UCMP used a scalar FACE condition");

   strarray_free(&shader, true);
}
END_TEST

static Suite *
init_suite(void)
{
   Suite *s;
   TCase *tc_core;

   s = suite_create("vrend_shader");
   tc_core = tcase_create("shader");

   suite_add_tcase(s, tc_core);
   tcase_add_test(tc_core, face_sources_are_vectors_for_compare_and_cmov);

   return s;
}

int
main(void)
{
   Suite *s;
   SRunner *sr;
   int number_failed;

   s = init_suite();
   sr = srunner_create(s);

   srunner_run_all(sr, CK_NORMAL);
   number_failed = srunner_ntests_failed(sr);
   srunner_free(sr);

   return number_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
