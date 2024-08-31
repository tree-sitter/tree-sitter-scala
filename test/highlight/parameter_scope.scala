def meth_with_params(localParam: Int) {
  var ref_param = c"$localParam $meth_with_params"
//                   ^parameter
//                               ^method
}
var okay1 = s"hello"
var okay = c"$localParam $okay1"
//            ^none
//                        ^variable
