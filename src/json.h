// json.h - a tiny JSON parser, just enough to read GitHub API replies.
#ifndef JSON_H
#define JSON_H

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JVal JVal;
struct JVal {
    int type;      // one of J_NULL ... J_OBJ
    char *key;     // name of this value when it lives inside an object
    char *str;     // text (J_STR)
    double num;    // number (J_NUM)
    int boolean;   // true/false (J_BOOL)
    JVal *child;   // first item (J_ARR / J_OBJ)
    JVal *next;    // next sibling in the parent array/object
};

JVal *json_parse(const char *text);          // returns NULL on bad JSON
void json_free(JVal *v);

JVal *json_get(JVal *obj, const char *key);  // look up a key in an object
const char *json_str(JVal *obj, const char *key, const char *def);
double json_num(JVal *obj, const char *key, double def);
int json_bool(JVal *obj, const char *key, int def);
int json_count(JVal *arr);                   // number of items in an array

#endif
