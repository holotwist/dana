#ifndef COMMAND_LINE_PARSER_H_INCLUDED
#define COMMAND_LINE_PARSER_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    COMMAND_LINE_PARSER_RESULT_OK,
    COMMAND_LINE_PARSER_RESULT_INVALID_ARGUMENT,
    COMMAND_LINE_PARSER_RESULT_INSUFFICIENT_OTHER_STRING_ARRAY_SIZE,
    COMMAND_LINE_PARSER_RESULT_NOT_SPECIFY_ARGUMENT_TO_OPTION,
    COMMAND_LINE_PARSER_RESULT_UNKNOWN_OPTION,
    COMMAND_LINE_PARSER_RESULT_OPTION_MULTIPLY_SPECIFIED,
    COMMAND_LINE_PARSER_RESULT_INVALID_SPECIFICATION,
    COMMAND_LINE_PARSER_RESULT_INVAILD_SHORT_OPTION_ARGUMENT
} CommandLineParserResult;

typedef enum {
    COMMAND_LINE_PARSER_FALSE = 0,
    COMMAND_LINE_PARSER_TRUE
} CommandLineParserBool;

struct CommandLineParserSpecification {
    char                  short_option;
    const char*           long_option;
    CommandLineParserBool need_argument;
    const char*           description;
    const char*           argument_string;
    CommandLineParserBool acquired;
};

#ifdef __cplusplus
extern "C" {
#endif

void CommandLineParser_PrintDescription(const struct CommandLineParserSpecification* clps);
CommandLineParserBool CommandLineParser_GetOptionAcquired(const struct CommandLineParserSpecification* clps, const char* option_name);
const char* CommandLineParser_GetArgumentString(const struct CommandLineParserSpecification* clps, const char* option_name);
CommandLineParserResult CommandLineParser_ParseArguments(
    struct CommandLineParserSpecification* restrict clps,
    int32_t argc, char** restrict argv,
    const char** restrict other_string_array, uint32_t other_string_array_size);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_LINE_PARSER_H_INCLUDED */