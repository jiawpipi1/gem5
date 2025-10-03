from m5.SimObject import SimObject
from m5.params import *

class ErrorModel(SimObject):
    type = 'ErrorModel'
    cxx_header = "mem/error_model.hh"   
    cxx_class = 'gem5::DRAMFailureModel'           
