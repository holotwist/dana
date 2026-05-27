#include "command_line_parser.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static uint32_t CommandLineParser_GetNumSpecifications(const struct CommandLineParserSpecification* clps) {
    assert(clps != NULL);
    uint32_t num_specs = 0;
    while (clps->short_option != 0) {
        num_specs++;
        clps++;
    }
    return num_specs;
}

static CommandLineParserBool CommandLineParser_CheckSpecification(const struct CommandLineParserSpecification* clps) {
    assert(clps != NULL);
    uint32_t num_specs = CommandLineParser_GetNumSpecifications(clps);

    for (uint32_t i = 0; i < num_specs; i++) {
        for (uint32_t j = 0; j < num_specs; j++) {
            if (i == j) continue;
            if (clps[j].short_option == clps[i].short_option) return COMMAND_LINE_PARSER_FALSE;
            if (clps[j].long_option && clps[i].long_option && strcmp(clps[j].long_option, clps[i].long_option) == 0) {
                return COMMAND_LINE_PARSER_FALSE;
            }
        }
    }
    return COMMAND_LINE_PARSER_TRUE;
}

void CommandLineParser_PrintDescription(const struct CommandLineParserSpecification* clps) {
    if (clps == NULL) {
        fprintf(stderr, "Pointer to command-line specification is NULL. \n");
        return;
    }

    if (CommandLineParser_CheckSpecification(clps) != COMMAND_LINE_PARSER_TRUE) {
        fprintf(stderr, "Warning: Command-line specification is invalid. (Unable to parse) \n");
    }

    uint32_t num_specs = CommandLineParser_GetNumSpecifications(clps);
    for (uint32_t i = 0; i < num_specs; i++) {
        const struct CommandLineParserSpecification* pspec = &clps[i];
        const char* arg_option_attr = (pspec->need_argument == COMMAND_LINE_PARSER_TRUE) ? "(needs argument)" : "";
        
        char command_str[256];
        if (pspec->long_option != NULL) {
            snprintf(command_str, sizeof(command_str), "  -%c, --%s", pspec->short_option, pspec->long_option);
        } else {
            snprintf(command_str, sizeof(command_str), "  -%c", pspec->short_option);
        }
        printf("%-20s %-18s  %s \n", command_str, arg_option_attr, pspec->description ? pspec->description : "");
    }
}

static CommandLineParserResult CommandLineParser_GetSpecificationIndex(const struct CommandLineParserSpecification* clps, const char* option_name, uint32_t* index) {
    if (!clps || !option_name || !index) return COMMAND_LINE_PARSER_RESULT_INVALID_ARGUMENT;

    uint32_t num_specs = CommandLineParser_GetNumSpecifications(clps);
    if (strlen(option_name) == 1) {
        for (uint32_t i = 0; i < num_specs; i++) {
            if (option_name[0] == clps[i].short_option) {
                *index = i;
                return COMMAND_LINE_PARSER_RESULT_OK;
            }
        }
    }

    for (uint32_t i = 0; i < num_specs; i++) {
        if (clps[i].long_option && strcmp(option_name, clps[i].long_option) == 0) {
            *index = i;
            return COMMAND_LINE_PARSER_RESULT_OK;
        }
    }
    return COMMAND_LINE_PARSER_RESULT_UNKNOWN_OPTION;
}

CommandLineParserBool CommandLineParser_GetOptionAcquired(const struct CommandLineParserSpecification* clps, const char* option_name) {
    uint32_t spec_no;
    if (CommandLineParser_GetSpecificationIndex(clps, option_name, &spec_no) != COMMAND_LINE_PARSER_RESULT_OK) {
        return COMMAND_LINE_PARSER_FALSE;
    }
    return clps[spec_no].acquired;
}

const char* CommandLineParser_GetArgumentString(const struct CommandLineParserSpecification* clps, const char* option_name) {
    uint32_t spec_no;
    if (CommandLineParser_GetSpecificationIndex(clps, option_name, &spec_no) != COMMAND_LINE_PARSER_RESULT_OK) {
        return NULL;
    }
    return clps[spec_no].argument_string;
}

CommandLineParserResult CommandLineParser_ParseArguments(
    struct CommandLineParserSpecification* restrict clps,
    int32_t argc, char** restrict argv,
    const char** restrict other_string_array, uint32_t other_string_array_size)
{
    if (!argv || !clps) return COMMAND_LINE_PARSER_RESULT_INVALID_ARGUMENT;

    uint32_t num_specs = CommandLineParser_GetNumSpecifications(clps);
    if (CommandLineParser_CheckSpecification(clps) != COMMAND_LINE_PARSER_TRUE) {
        return COMMAND_LINE_PARSER_RESULT_INVALID_SPECIFICATION;
    }

    for (uint32_t i = 0; i < num_specs; i++) {
        clps[i].acquired = COMMAND_LINE_PARSER_FALSE;
    }

    uint32_t other_string_index = 0;
    for (int32_t count = 1; count < argc; count++) {
        const char* arg_str = argv[count];

        if (strncmp(arg_str, "--", 2) == 0) {
            uint32_t spec_no;
            for (spec_no = 0; spec_no < num_specs; spec_no++) {
                struct CommandLineParserSpecification* pspec = &clps[spec_no];
                if (!pspec->long_option) continue;

                uint32_t long_option_len = (uint32_t)strlen(pspec->long_option);
                if (strncmp(&arg_str[2], pspec->long_option, long_option_len) == 0) {
                    if (arg_str[2 + long_option_len] == '\0') {
                        if (pspec->acquired == COMMAND_LINE_PARSER_TRUE) {
                            fprintf(stderr, "%s: Option \"%s\" multiply specified. \n", argv[0], pspec->long_option);
                            return COMMAND_LINE_PARSER_RESULT_OPTION_MULTIPLY_SPECIFIED;
                        }
                        if (pspec->need_argument == COMMAND_LINE_PARSER_TRUE) {
                            if ((count + 1) == argc || strncmp(argv[count + 1], "--", 2) == 0 || argv[count + 1][0] == '-') {
                                fprintf(stderr, "%s: Option \"%s\" needs argument. \n", argv[0], pspec->long_option);
                                return COMMAND_LINE_PARSER_RESULT_NOT_SPECIFY_ARGUMENT_TO_OPTION;
                            }
                            pspec->argument_string = argv[++count];
                        }
                    } else if (arg_str[2 + long_option_len] == '=') {
                        if (pspec->need_argument != COMMAND_LINE_PARSER_TRUE) continue;
                        if (pspec->acquired == COMMAND_LINE_PARSER_TRUE) {
                            fprintf(stderr, "%s: Option \"%s\" multiply specified. \n", argv[0], pspec->long_option);
                            return COMMAND_LINE_PARSER_RESULT_OPTION_MULTIPLY_SPECIFIED;
                        }
                        pspec->argument_string = &arg_str[2 + long_option_len + 1];
                    } else {
                        continue;
                    }
                    pspec->acquired = COMMAND_LINE_PARSER_TRUE;
                    break;
                }
            }
            if (spec_no == num_specs) {
                fprintf(stderr, "%s: Unknown long option - \"%s\" \n", argv[0], &arg_str[2]);
                return COMMAND_LINE_PARSER_RESULT_UNKNOWN_OPTION;
            }
        } else if (arg_str[0] == '-') {
            for (uint32_t str_index = 1; arg_str[str_index] != '\0'; str_index++) {
                uint32_t spec_no;
                for (spec_no = 0; spec_no < num_specs; spec_no++) {
                    struct CommandLineParserSpecification* pspec = &clps[spec_no];
                    if (arg_str[str_index] == pspec->short_option) {
                        if (pspec->acquired == COMMAND_LINE_PARSER_TRUE) {
                            fprintf(stderr, "%s: Option \'%c\' multiply specified. \n", argv[0], pspec->short_option);
                            return COMMAND_LINE_PARSER_RESULT_OPTION_MULTIPLY_SPECIFIED;
                        }
                        if (pspec->need_argument == COMMAND_LINE_PARSER_TRUE) {
                            if (arg_str[str_index + 1] != '\0') {
                                fprintf(stderr, "%s: Option \'%c\' needs argument. Please specify tail of short option sequence.\n", argv[0], pspec->short_option);
                                return COMMAND_LINE_PARSER_RESULT_INVAILD_SHORT_OPTION_ARGUMENT;
                            }
                            if ((count + 1) == argc || strncmp(argv[count + 1], "--", 2) == 0 || argv[count + 1][0] == '-') {
                                fprintf(stderr, "%s: Option \'%c\' needs argument. \n", argv[0], pspec->short_option);
                                return COMMAND_LINE_PARSER_RESULT_NOT_SPECIFY_ARGUMENT_TO_OPTION;
                            }
                            pspec->argument_string = argv[++count];
                        }
                        pspec->acquired = COMMAND_LINE_PARSER_TRUE;
                        break;
                    }
                }
                if (spec_no == num_specs) {
                    fprintf(stderr, "%s: Unknown short option - \'%c\' \n", argv[0], arg_str[str_index]);
                    return COMMAND_LINE_PARSER_RESULT_UNKNOWN_OPTION;
                }
            }
        } else {
            if (other_string_array == NULL) return COMMAND_LINE_PARSER_RESULT_INVALID_ARGUMENT;
            if (other_string_index >= other_string_array_size) {
                fprintf(stderr, "%s: Too many strings specified. \n", argv[0]);
                return COMMAND_LINE_PARSER_RESULT_INSUFFICIENT_OTHER_STRING_ARRAY_SIZE;
            }
            other_string_array[other_string_index++] = arg_str;
        }
    }
    return COMMAND_LINE_PARSER_RESULT_OK;
}