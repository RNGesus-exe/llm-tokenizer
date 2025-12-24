MAIN_APP := tokenizer

CC := gcc
CFLAGS := -Wall -Wextra -Werror -O0

SRC = bpe

default: $(MAIN_APP)

$(MAIN_APP): $(SRC).c
	$(CC) $(CFLAGS) $(SRC).c -o $(MAIN_APP)

clean:
	-rm -f $(MAIN_APP)

.PHONY: clean