#include "model_data.h"

#include "model_weights.h"

const tinyml_model_t *model_data_get(void)
{
    return &TINYML_MODEL;
}

const char *model_data_version(void)
{
    return TINYML_MODEL.version;
}

uint32_t model_data_size_bytes(void)
{
    return TINYML_MODEL.model_bytes;
}
