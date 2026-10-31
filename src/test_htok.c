/*
 * test_htok.c — Unit tests for native C HTOK v4 packing and unpacking
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "ttypt/corm.h"

static void test_basic_pack_unpack(void) {
	printf("=== Test 1: Basic HTOK v4 pack and unpack ===\n");

	const char *json_in =
		"{"
		"\"level\":\"L1\","
		"\"type\":\"episode\","
		"\"lifecycle\":\"supported\","
		"\"summary\":\"Fix audio latency in FLAC pipeline\","
		"\"timestamp\":\"2026-03-30T12:00:00Z\","
		"\"standing\":false,"
		"\"evidence\":[1, 5, 12],"
		"\"tags\":[\"audio\", \"flac\", \"latency\"],"
		"\"context\":{\"project\":\"site\",\"subsystem\":\"audio\"},"
		"\"episode\":{"
			"\"problem\":\"High buffer latency on 48kHz audio\","
			"\"outcome\":\"success\","
			"\"findings\":[\"Ring buffer size was throttled to 64 samples\"],"
			"\"diffs\":[\"src/audio.c\", \"include/audio.h\"],"
			"\"approaches\":["
				"{\"outcome\":\"failed\",\"approach\":\"Increase OS buffer\",\"reason\":\"Caused underrun\"},"
				"{\"outcome\":\"worked\",\"approach\":\"Adjust ring buffer chunk to 256\",\"reason\":\"Latency reduced to 4ms\"}"
			"],"
			"\"sessionId\":\"12345678-1234-5678-1234-567812345678\""
		"}"
		"}";

	uint8_t *bin = NULL;
	size_t bin_len = 0;
	int rc = corm_htok_pack(json_in, strlen(json_in), &bin, &bin_len);
	assert(rc == 0);
	assert(bin != NULL);
	assert(bin_len > 12);

	/* Verify Header */
	uint32_t magic = ((uint32_t)bin[0]) | (((uint32_t)bin[1]) << 8) | (((uint32_t)bin[2]) << 16) | (((uint32_t)bin[3]) << 24);
	assert(magic == CORM_HTOK_MAGIC);

	uint16_t version = ((uint16_t)bin[4]) | (((uint16_t)bin[5]) << 8);
	assert(version == 4);

	uint8_t triad = bin[10];
	uint8_t lvl = (triad >> 6) & 3;
	uint8_t lc = (triad >> 3) & 7;
	uint8_t tp = triad & 7;
	assert(lvl == 1); /* L1 */
	assert(lc == 3);  /* supported */
	assert(tp == 0);  /* episode */

	uint8_t flags = bin[11];
	assert(flags & CORM_HTOK_FLAG_HAS_EVIDENCE);

	/* Trailer */
	assert(bin[bin_len - 1] == 0xaa);
	printf("  Packed binary size: %zu bytes (clean HTOK v4)\n", bin_len);

	/* Unpack */
	char *json_out = NULL;
	size_t json_out_len = 0;
	rc = corm_htok_unpack(bin, bin_len, &json_out, &json_out_len);
	assert(rc == 0);
	assert(json_out != NULL);
	assert(json_out_len > 0);

	printf("  Unpacked JSON: %s\n", json_out);

	/* Verify JSON fields */
	assert(strstr(json_out, "\"level\":\"L1\"") != NULL);
	assert(strstr(json_out, "\"type\":\"episode\"") != NULL);
	assert(strstr(json_out, "\"lifecycle\":\"supported\"") != NULL);
	assert(strstr(json_out, "\"summary\":\"Fix audio latency in FLAC pipeline\"") != NULL);
	assert(strstr(json_out, "\"evidence\":[1,5,12]") != NULL);
	assert(strstr(json_out, "\"project\":\"site\"") != NULL);
	assert(strstr(json_out, "\"subsystem\":\"audio\"") != NULL);
	assert(strstr(json_out, "\"problem\":\"High buffer latency on 48kHz audio\"") != NULL);
	assert(strstr(json_out, "\"outcome\":\"success\"") != NULL);
	assert(strstr(json_out, "\"Ring buffer size was throttled to 64 samples\"") != NULL);
	assert(strstr(json_out, "\"src/audio.c\"") != NULL);
	assert(strstr(json_out, "\"Adjust ring buffer chunk to 256\"") != NULL);
	assert(strstr(json_out, "12345678-1234-5678-1234-567812345678") != NULL);

	free(bin);
	free(json_out);
	printf("  PASS\n");
}

static void test_standing_directive_roundtrip(void) {
	printf("=== Test 2: Standing directive L2 invariant roundtrip ===\n");

	const char *json_in =
		"{"
		"\"level\":\"L2\","
		"\"type\":\"invariant\","
		"\"lifecycle\":\"supported\","
		"\"summary\":\"All HTTP handlers must return structured error frames\","
		"\"standing\":true,"
		"\"supersedes\":42,"
		"\"tags\":[\"convention\", \"error-handling\"],"
		"\"context\":{\"project\":\"site\"}"
		"}";

	uint8_t *bin = NULL;
	size_t bin_len = 0;
	int rc = corm_htok_pack(json_in, 0, &bin, &bin_len);
	assert(rc == 0);

	/* Unpack */
	char *json_out = NULL;
	size_t json_out_len = 0;
	rc = corm_htok_unpack(bin, bin_len, &json_out, &json_out_len);
	assert(rc == 0);

	assert(strstr(json_out, "\"level\":\"L2\"") != NULL);
	assert(strstr(json_out, "\"type\":\"invariant\"") != NULL);
	assert(strstr(json_out, "\"standing\":true") != NULL);
	assert(strstr(json_out, "\"supersedes\":42") != NULL);
	assert(strstr(json_out, "\"All HTTP handlers must return structured error frames\"") != NULL);

	free(bin);
	free(json_out);
	printf("  PASS\n");
}

static void test_error_handling(void) {
	printf("=== Test 3: Error handling ===\n");

	/* Null input */
	assert(corm_htok_pack(NULL, 0, NULL, NULL) < 0);
	assert(corm_htok_unpack(NULL, 0, NULL, NULL) < 0);

	/* Invalid JSON */
	uint8_t *bin = NULL;
	size_t bin_len = 0;
	assert(corm_htok_pack("not json", 8, &bin, &bin_len) < 0);

	/* Truncated HTOK */
	uint8_t bad_htok[] = {'H', 'T', 'O', 'K'};
	char *out = NULL;
	size_t out_len = 0;
	assert(corm_htok_unpack(bad_htok, sizeof(bad_htok), &out, &out_len) < 0);

	printf("  PASS\n");
}

int main(void) {
	test_basic_pack_unpack();
	test_standing_directive_roundtrip();
	test_error_handling();
	printf("\nALL HTOK TESTS PASSED!\n");
	return 0;
}
