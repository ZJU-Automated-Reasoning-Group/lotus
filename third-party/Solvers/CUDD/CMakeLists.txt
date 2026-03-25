add_library(CanaryCUDD STATIC
	cuddTable.c
	cuddInit.c
	cuddAPI.c
	cuddRef.c
	cuddCheck.c
	cuddUtil.c
	cuddCof.c
	cuddSat.c
	cuddReorder.c
	cuddAddIte.c
	cuddBddIte.c
	cuddBddAbs.c
	cuddZddReord.c
	safe_mem.c
	mtrBasic.c
	mtrGroup.c
	cuddCache.c
	cuddLCache.c
	cuddInteract.c
	cuddGroup.c
	cuddAnneal.c
	cuddExact.c
	cuddGenetic.c
	cuddLinear.c
	cuddSymmetry.c
	cuddWindow.c
	cuddZddFuncs.c
	cuddZddGroup.c
	cuddZddIsop.c
	cuddZddSetop.c
	cuddZddLin.c
	cuddZddSymm.c
	datalimit.c
	cpu_time.c
	epd.c
	st.c
)

set_target_properties(CanaryCUDD PROPERTIES COMPILE_FLAGS "-Wno-format -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -DHAVE_IEEE_754 -DSIZEOF_VOID_P=8 -DSIZEOF_LONG=8")

# Consumers use "Solvers/CUDD/cudd.h" with third-party on include path
target_include_directories(CanaryCUDD PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
	$<INSTALL_INTERFACE:include>
)
