# Merge component inventories while retaining one version/target/profile header.
file(READ "${INPUT1}" inventory)
file(READ "${INPUT2}" additional)
string(REGEX REPLACE "^[^\n]*\n[^\n]*\n[^\n]*\n" "" additional "${additional}")
file(WRITE "${OUTPUT}" "${inventory}${additional}")
