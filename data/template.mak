# Compiler and flags
FF = gfortran
FFLAGS = -cpp -fno-align-commons -O3 -ffpe-trap=zero,invalid,underflow,overflow \
         -std=legacy -ffixed-line-length-none -fall-intrinsics \
         -Wno-unused-variable -Wno-unused-function -Wno-conversion -fopenmp
FMOD = -Jmod -Imod
PROGRAM = prop_sp

# Directories
SRC_DIR = src
OBJ_DIR = obj
MOD_DIR = mod

# Topologically sorted module sources looking recursively through a source directory
TOPOLOGIC_SRC = $(shell build/maketopologicf90 -D $(SRC_DIR))

# Corresponding object files
OBJECTS = $(patsubst $(SRC_DIR)/%.f90, $(OBJ_DIR)/%.o, \
          $(patsubst $(SRC_DIR)/%.for, $(OBJ_DIR)/%.o, $(TOPOLOGIC_SRC)))

# Link target
all: $(PROGRAM)

# Build the program
$(PROGRAM): $(OBJECTS)
	$(FF) $(FFLAGS) -o $@ $^

# Generic pattern rule for both .f90 and .for files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.f90
	@mkdir -p $(dir $@) $(MOD_DIR)
	$(FF) $(FFLAGS) $(FMOD) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.for
	@mkdir -p $(dir $@) $(MOD_DIR)
	$(FF) $(FFLAGS) $(FMOD) -c $< -o $@

# Clean rule
clean:
	rm -rf $(OBJ_DIR)/*.o $(MOD_DIR)/*.mod $(PROGRAM)