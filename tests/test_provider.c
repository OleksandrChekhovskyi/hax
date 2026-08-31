/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "provider.h"
#include "xalloc.h"

static struct item make_image_item(size_t base64_bytes)
{
    struct item_image *image = xcalloc(1, sizeof(*image));
    image->mime = xstrdup("image/png");
    image->data_b64 = xmalloc(base64_bytes + 1);
    memset(image->data_b64, 'A', base64_bytes);
    image->data_b64[base64_bytes] = '\0';
    return (struct item){.kind = ITEM_TOOL_RESULT, .images = image, .n_images = 1};
}

static void test_item_image_totals(void)
{
    struct item items[] = {make_image_item(1000),
                           {.kind = ITEM_USER_MESSAGE},
                           make_image_item(2500),
                           {.kind = ITEM_ASSISTANT_MESSAGE}};

    EXPECT(items_image_base64_bytes(items, 4) == 3500);
    EXPECT(items_image_count(items, 4) == 2);

    struct item no_images[] = {{.kind = ITEM_USER_MESSAGE}, {.kind = ITEM_ASSISTANT_MESSAGE}};
    EXPECT(items_image_base64_bytes(no_images, 2) == 0);
    EXPECT(items_image_count(no_images, 2) == 0);

    for (size_t i = 0; i < 4; i++)
        item_free(&items[i]);
}

static void test_item_image_placeholder(void)
{
    struct item_image image = {
        .mime = "image/png",
        .data_b64 = "AAAA",
        .width = 640,
        .height = 480,
    };
    char *placeholder = item_image_placeholder(&image);
    EXPECT_STR_EQ(placeholder, "[image: image/png, 640x480, 3 bytes]");
    free(placeholder);

    memset(&image, 0, sizeof(image));
    placeholder = item_image_placeholder(&image);
    EXPECT_STR_EQ(placeholder, "[image: image, 0 bytes]");
    free(placeholder);
}

static void test_item_free(void)
{
    struct item item = make_image_item(4);
    item.text = xstrdup("text");
    item.call_id = xstrdup("call");
    item.tool_name = xstrdup("read");
    item.tool_arguments_json = xstrdup("{}");
    item.output = xstrdup("output");
    item.reasoning_json = xstrdup("{}");
    item.reasoning_text = xstrdup("reasoning");
    item.provider = xstrdup("provider");
    item.model = xstrdup("model");
    item.usage = xcalloc(1, sizeof(*item.usage));

    item_free(&item);
    item_free(NULL);
}

static void test_model_info_lifecycle(void)
{
    struct model_info source;
    model_info_init(&source);
    EXPECT(source.cost_input < 0 && source.cost_cache_read < 0 && source.cost_output < 0);
    EXPECT(source.cost_cache_write < 0 && source.cost_cache_write_1h < 0);

    source.id = xstrdup("model");
    source.description = xstrdup("description");
    source.context = 1000;
    source.image_input = PROVIDER_CAP_YES;

    struct model_info copy;
    model_info_copy(&copy, &source);
    EXPECT_STR_EQ(copy.id, source.id);
    EXPECT_STR_EQ(copy.description, source.description);
    EXPECT(copy.id != source.id && copy.description != source.description);
    EXPECT(copy.context == 1000 && copy.image_input == PROVIDER_CAP_YES);

    model_info_clear(&source);
    model_info_clear(&copy);
}

static char **make_headers(void)
{
    char **headers = xcalloc(2, sizeof(*headers));
    headers[0] = xstrdup("Authorization: secret");
    return headers;
}

static void test_request_cleanup(void)
{
    struct model_probe probe = {
        .url = xstrdup("https://example.test/model"),
        .headers = make_headers(),
        .timeout_s = 5,
    };
    model_probe_clear(&probe);
    EXPECT(probe.url == NULL && probe.headers == NULL && probe.timeout_s == 0);

    struct provider_availability availability = {
        .available = 1,
        .reason = xstrdup("out of coffee"),
        .url = xstrdup("https://example.test"),
        .headers = make_headers(),
        .timeout_s = 5,
    };
    provider_availability_clear(&availability);
    EXPECT(availability.available == 0 && availability.reason == NULL);
    EXPECT(availability.url == NULL && availability.headers == NULL && availability.timeout_s == 0);

    model_probe_clear(NULL);
    provider_availability_clear(NULL);
}

static void test_provenance_matching(void)
{
    /* The former llamacpp id maps to the current one; everything else passes through. */
    EXPECT_STR_EQ(provider_canonical_id("llama.cpp"), "llamacpp");
    EXPECT_STR_EQ(provider_canonical_id("openai"), "openai");
    EXPECT(provider_canonical_id(NULL) == NULL);

    struct item item = {.kind = ITEM_REASONING, .provider = "llamacpp", .model = "m1"};
    EXPECT(provider_provenance_matches(&item, "llamacpp", "m1"));
    EXPECT(!provider_provenance_matches(&item, "llamacpp", "m2"));
    EXPECT(!provider_provenance_matches(&item, "openai", "m1"));

    /* A session written before the llamacpp rename replays its reasoning after resume. */
    item.provider = "llama.cpp";
    EXPECT(provider_provenance_matches(&item, "llamacpp", "m1"));

    /* Display-name provenance from older renamed setups never matches a stable id. */
    item.provider = "vLLM";
    EXPECT(!provider_provenance_matches(&item, "openai-compatible", "m1"));

    item.provider = NULL;
    EXPECT(!provider_provenance_matches(&item, "llamacpp", "m1"));
}

int main(void)
{
    test_item_image_totals();
    test_item_image_placeholder();
    test_item_free();
    test_model_info_lifecycle();
    test_request_cleanup();
    test_provenance_matching();
    T_REPORT();
}
