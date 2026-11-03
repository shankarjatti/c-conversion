CC = g++
CFLAGS = -fmax-errors=5 -O2 -std=c++11 -Wall -Wextra -fPIC -I. -I../tetra-kit/decoder
LDFLAGS = -lz -shared

TETRAKIT = ../tetra-kit/decoder
OBJDIR = pic_objs

SRC = capi_shim.cc \
	$(TETRAKIT)/decoder.cc \
	$(TETRAKIT)/common/base64.cc $(TETRAKIT)/common/pdu.cc $(TETRAKIT)/common/layer.cc \
	$(TETRAKIT)/common/log.cc $(TETRAKIT)/common/report.cc $(TETRAKIT)/common/tetracell.cc \
	$(TETRAKIT)/common/utils.cc $(TETRAKIT)/common/tetra.cc \
	$(TETRAKIT)/llc/llc.cc \
	$(TETRAKIT)/mle/mle.cc $(TETRAKIT)/mle/mle_elements.cc \
	$(TETRAKIT)/sndcp/sndcp.cc \
	$(TETRAKIT)/cmce/cmce.cc $(TETRAKIT)/cmce/sds/sds.cc $(TETRAKIT)/cmce/sds/lip.cc \
	$(TETRAKIT)/mm/mm.cc $(TETRAKIT)/mm/mm_elements.cc $(TETRAKIT)/mm/d_authentication.cc \
	$(TETRAKIT)/mm/d_mm_status.cc $(TETRAKIT)/mm/d_otar.cc \
	$(TETRAKIT)/uplane/uplane.cc \
	$(TETRAKIT)/mac/coding.cc $(TETRAKIT)/mac/macdefrag.cc $(TETRAKIT)/mac/mac.cc $(TETRAKIT)/mac/viterbi.cc \
	$(TETRAKIT)/wiremsg/wiremsg.cc

# Build into our own pic_objs/ tree, mirroring tetra-kit's layout, instead of
# tetra-kit's own .o files — those were built without -fPIC for the standalone
# `decoder` binary and must stay untouched.
OBJ = $(patsubst %.cc,$(OBJDIR)/%.o,$(SRC))
LIB = libtetrakit_capi.so

all: $(LIB)

$(LIB): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJDIR) $(LIB)
