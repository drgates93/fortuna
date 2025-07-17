#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include "fortuna_levenshtein.h"
#include "fortuna_helper_fn.h"


// Trie node with bitmask and children pointers
typedef struct TrieNode {
    uint32_t mask;                 // 27-bit mask for children presence
    struct TrieNode* children[ALPHABET_SIZE];
    int is_word;
} TrieNode;


//Arena allocated Trie. This reduces overhead as much as possible.
#define MAX_NODES 256
static TrieNode arena[MAX_NODES];
static int arena_idx = 0;

//Check the input is valid.
static inline int is_valid_query(const char* word) {
    while (*word) {
        if (*word != '-' && !islower((unsigned char)*word)) {
            return 0; // invalid character
        }
        word++;
    }
    return 1; // all characters are valid
}

// Map char to 0..26 index (a-z + '-')
static inline int char_index(char c) {
    return (c == '-') ? 26 : (c - 'a'); // assume valid input only
}

//Allocated a new Trie node in the arena. No real allocation, but sets the memory up.
TrieNode* alloc_node(void) {
    if (arena_idx >= MAX_NODES) return NULL;
    TrieNode* node = &arena[arena_idx++];
    node->mask = 0;
    node->is_word = 0;
    memset(node->children, 0, sizeof(node->children));
    return node;
}

// Insert word into trie
void insert_word(TrieNode* root, const char* word) {
    while (*word) {
        int idx = char_index(*word);
        uint32_t bit = 1u << idx;

        if (!(root->mask & bit)) {
            root->mask |= bit;
            root->children[idx] = alloc_node();
        }

        root = root->children[idx];
        word++;
    }
    root->is_word = 1;
}

//Check if the prefixes don't match to some count. Skip if they dont. 
int prefix_mismatch(const char *a, const char *b, int count) {
    for (int i = 0; i < count; i++) {
        if (a[i] == '\0' || b[i] == '\0') return 0;
        if (a[i] != b[i]) return 1;  // mismatch
    }
    return 0; // match
}

//Min of 3 numbers as ternary operators. 
static inline int min3(int a, int b, int c) {
    return (a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c);
}


void search_recursive(TrieNode* node,
                     const char* target,
                     int len,
                     int max_dist,
                     int dp[MAX_WORD_LEN + 1][MAX_WORD_LEN + 1],
                     char* current_word,
                     char* best_word,
                     int depth,
                     int *best_score) {

    //Return if we hit an empty node or the depth is exceeded.
    if (!node || depth >= MAX_WORD_LEN) return;

    int* prev_row = dp[depth];
    int* curr_row = dp[depth + 1];

    //Variable declarations
    char ch;
    int i,min_cost,cost;

    //Get the mask for what nodes are not-empty. 
    uint32_t mask = node->mask;
    while (mask) {
        i = __builtin_ctz(mask);
        mask &= mask - 1;

        ch = (i == 26) ? '-' : ('a' + i);
        current_word[depth] = ch;
        current_word[depth + 1] = '\0';

        // Reject mismatch for first character. 
        if (depth == 0 && current_word[depth] != target[depth]) continue;

        //Update the rows 
        curr_row[0]  = prev_row[0] + 1;
        min_cost = curr_row[0];

        for (int j = 1; j <= len; ++j) {
            cost = (target[j - 1] != ch);
            curr_row[j] = min3(
                curr_row[j - 1] + 1,    // insertion
                prev_row[j] + 1,        // deletion
                prev_row[j - 1] + cost  // substitution
            );
            if (curr_row[j] < min_cost) {
                min_cost = curr_row[j];
            }
        }

        //Update the pointer
        TrieNode* child = node->children[i];

        if (child && child->is_word) {
            if (min_cost <= max_dist && min_cost < *best_score) {
                strcpy(best_word, current_word);
                *best_score = min_cost;
            }
        }

        //Only execute the recursion if this branch could be better. 
        if (min_cost <= max_dist) {
            search_recursive(child, target, len, max_dist, dp,
                             current_word, best_word, depth + 1, best_score);
        }
    }
}

    
static const char* dictionary[MAX_WORDS] = {"new",
                                            "build",
                                            "run",
                                            "--lib",
                                            "--bin",
                                            "--rebuild",
                                            "-r",
                                            "-j"};
static const int dictSize = 8;

void loadDictionary(TrieNode *root) {
    for(int i = 0; i < dictSize; i++) {
        insert_word(root, dictionary[i]);
    }
}

void freeTrie(TrieNode *root) {
    if (!root) return;
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (root->children[i]) {
            freeTrie(root->children[i]);
        }
    }
    free(root);
}


static int dp[MAX_WORD_LEN + 1][MAX_WORD_LEN + 1];
int suggest_closest_word_fuzzy(TrieNode *root, const char *input) {
    int len = (int)strlen(input);
    if (len >= MAX_WORD_LEN) {
        print_error("Input too long\n");
        return -1;
    }

    //Validate the string
    if(!is_valid_query(input)){
        char msg[256];
        snprintf(msg,sizeof(msg),"Unknown flag");
        print_error(msg);
        return -1;
    }

    // Initialize first row of Levenshtein DP = distance from empty string
    for (int i = 0; i <= len; i++) dp[0][i] = i;

    int best_score = INT_MAX;
    char best_match[MAX_WORD_LEN + 1];
    best_match[0] = '\0';

    char prefix[MAX_WORD_LEN];
    prefix[0] = '\0';

    int max_distance = (len < 2) ? 2 : len;

    //Search the Trie.
    search_recursive(root, input, len, max_distance, dp, prefix, 
                     best_match, 0, &best_score);

    //Perfect match so nothing to suggest.
    if(best_score == 0) return 0;

    printf("%s %s %d\n",input,best_match,best_score);

    //Print the suggestion if less than 3 away
    if(best_score <= 3){
        char msg[256];
        snprintf(msg,sizeof(msg),"Unknown flag: Did you mean: %s?",best_match);
        print_error(msg);
    }else{
        char msg[256];
        snprintf(msg,sizeof(msg),"Unknown flag");
        print_error(msg);
    }
    return -1;
}

int edit_distance_weighted(const char *a, const char *b, int weight) {
    int m = strlen(a), n = strlen(b);
    if (m == 0) return n;
    if (n == 0) return m;

    int dp_linear[MAX_WORD_LEN + 1];
    int prev, temp;

    for (int j = 0; j <= n; j++)
        dp_linear[j] = j * ((j <= 2) ? weight : 1);

    for (int i = 1; i <= m; i++) {
        prev = dp_linear[0];
        dp_linear[0] = i * ((i <= 2) ? weight : 1);

        for (int j = 1; j <= n; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 :
                       ((i <= 2 || j <= 2) ? weight : 1);
            temp = dp_linear[j];
            dp_linear[j] = min3(
                dp_linear[j]     + ((i <= 2) ? weight : 1), // Deletion
                dp_linear[j - 1] + ((j <= 2) ? weight : 1), // Insertion
                prev      + cost                     // Substitution
            );
            prev = temp;
        }
    }
    return dp_linear[n];
}

int suggest_closest_word_fuzzy_linear(const char *input) {
    int best_score = INT_MAX;
    const char *best_match = NULL;

    int firstCharWeight = 2;

    for (size_t i = 0; i < dictSize; i++) {
        int score = edit_distance_weighted(input, dictionary[i], firstCharWeight);
        if (score < best_score) {
            best_score = score;
            best_match = dictionary[i];
            if (score == 0) break; // Early exit on perfect match
        }
    }

    if(best_score == 0) return 0;

    //Print the suggestion if less than 3 away
    if(0 < best_score && best_score <= 4){
        char msg[256];
        snprintf(msg,sizeof(msg),"Unknown flag: Did you mean: %s?",best_match);
        print_error(msg);
    }else{
        char msg[256];
        snprintf(msg,sizeof(msg),"Unknown flag");
        print_error(msg);
    }
    return -1;
}


void random_edit(const char* src, char* out, int max_len) {
    int len = strlen(src);
    len = len > max_len - 1 ? max_len - 1 : len;
    strcpy(out, src);

    int edit_type = rand() % 3;
    int pos = rand() % (len ? len : 1);
    char new_ch = "abcdefghijklmnopqrstuvwxyz-"[rand() % 27];

    switch (edit_type) {
        case 0: // substitution
            out[pos] = new_ch;
            break;
        case 1: // deletion
            if (len > 1) memmove(&out[pos], &out[pos + 1], len - pos);
            break;
        case 2: // insertion
            if (len < max_len - 2) {
                memmove(&out[pos + 1], &out[pos], len - pos + 1);
                out[pos] = new_ch;
            }
            break;
    }
}

// Time utility
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void levenshtein_timing(int trials) {
    char query[MAX_WORD_LEN + 1];
    uint64_t total_linear = 0, total_trie = 0;

    // Build trie
    TrieNode* root = alloc_node();
    loadDictionary(root);

    for (int t = 0; t < trials; t++) {
        const char* base = dictionary[rand() % dictSize];
        random_edit(base, query, MAX_WORD_LEN);

        uint64_t t0 = now_ns();
        suggest_closest_word_fuzzy_linear(query);
        uint64_t t1 = now_ns();
        total_linear += (t1 - t0);

        uint64_t t2 = now_ns();
        suggest_closest_word_fuzzy(root,query);
        uint64_t t3 = now_ns();
        total_trie += (t3 - t2);
    }

    char msg[256];
    snprintf(msg,sizeof(msg),"Linear search total: %llu ns, avg: %.2f ns",total_linear, total_linear / (double)trials);
    print_test(msg);

    snprintf(msg,sizeof(msg),"Trie search   total: %llu ns, avg: %.2f ns",total_trie, total_trie / (double)trials);
    print_test(msg);
}