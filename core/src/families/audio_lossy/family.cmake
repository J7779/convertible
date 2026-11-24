# audio_lossy family: each codec area keeps its imported-library fragment in
# cmake/<area>.cmake (all of them are included here, in sorted order).
file(GLOB _cvt_lossy_parts "${CVT_FAMILY_DIR}/cmake/*.cmake")
list(SORT _cvt_lossy_parts)
foreach(_cvt_lossy_part IN LISTS _cvt_lossy_parts)
  include("${_cvt_lossy_part}")
endforeach()
unset(_cvt_lossy_parts)
