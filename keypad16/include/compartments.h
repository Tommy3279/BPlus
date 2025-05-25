#ifndef COMPARTMENTS_H_
#define COMPARTMENTS_H_

typedef enum {
    OK_STATE,
    FULL_STATE,
    FAULT_STATE
} compartment_state_t;

typedef struct {
    char name;                  // Compartment name (e.g., 'A', 'B', ...)
    compartment_state_t state;  // Current state of the compartment
} compartment_t;
// Number of compartments
#define NUM_COMPARTMENTS 4  // Example: 4 compartments (A, B, C, D)

// Declare compartments array as extern
extern compartment_t compartments[];

#endif /* COMPARTMENTS_H_ */