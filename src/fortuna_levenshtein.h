#ifndef FORTUNA_LEVENSHTEIN
#define FORTUNA_LEVENSHTEIN



#define MAX_WORD_LEN  64
#define ALPHABET_SIZE 27
#define MAX_WORDS     64

// Trie node structure (opaque to users)
typedef struct TrieNode TrieNode;

// Create a new empty trie node
TrieNode* alloc_node(void);

// Insert a word into the trie
void insert(TrieNode *root, const char *word);

// Load multiple words into the trie from an array of strings
void loadDictionary(TrieNode *root);

// Suggest the closest matching word in the trie to the input string
// Returns a newly allocated string that must be freed by the caller,
// or NULL if no match found.
int suggest_closest_word_fuzzy(TrieNode *root, const char *input);

//This does a linear search. Should be faster for out small dictionary, but the 
//full-up solution was fun to code. 
int suggest_closest_word_fuzzy_linear(const char *input);

// Free all memory associated with the trie
void freeTrie(TrieNode *root);

//Timing (and then testing) trials.
void levenshtein_timing(int trials);

#endif
