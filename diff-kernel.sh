#!/bin/sh
[ -n "$REPO_ROOT" ] || REPO_ROOT=$(pwd)

[ -n "$KERNEL_A" ] || KERNEL_A=linux-4.1.38
KERNEL_A_DIR="$REPO_ROOT/dl/$KERNEL_A"
echo "a <- $KERNEL_A_DIR"
[ -d ${KERNEL_A_DIR} ] || {
  echo -n "Preparing original kernel... "
  [ -f ${KERNEL_A_DIR}.tar.xz ] || {
    echo -e "failed\n\tmissing tarball for KERNEL_A at ${KERNEL_A_DIR}.tar.xz" && return
  }
  tar -xf ${KERNEL_A_DIR}.tar.xz -C ${REPO_ROOT}/dl && echo "done"
}

[ -n "$KERNEL_B" ] || KERNEL_B=linux-4.1
KERNEL_B_DIR="${ARCHIVE_ROOT}/kernel/$KERNEL_B"
echo "b <- ${KERNEL_B_DIR}"
echo -n "Diffing kernel... "
[ -d ${KERNEL_B_DIR} ] || {
  echo -e "failed\n\tKERNEL_B (${KERNEL_B}) not found at ${KERNEL_B_DIR}" && return
}

rm a b
ln -s ${KERNEL_A_DIR} a
ln -s ${KERNEL_B_DIR} b

echo
diff -ruN --no-dereference a/ b/ | tee /tmp/_generating_bcm.patch | stdbuf -o0 grep -e "+++" --binary-files=text --color=yes \
                                 | stdbuf -o0 cut -f 1 | stdbuf -o0 paste -sd "\r" && echo "done"
_output_patch=$REPO_ROOT/target/linux/brcm63xx-tch/000-Broadcom_bcm963xx_sdk_$(md5sum /tmp/_generating_bcm.patch|cut -c -6).patch
mv -f /tmp/_generating_bcm.patch $_output_patch

