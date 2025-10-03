CMAKE=cmake
CTEST=ctest
DIR=out

.PHONY: clean

all: compile

$(DIR):
	$(CMAKE) -B $(DIR)

compile: $(DIR)
	$(MAKE) -C $(DIR)

clean:
	rm -rf $(DIR)
