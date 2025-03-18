package com.example
//      ^definition.module

enum Color:
//   ^definition.enum
  case Red
  //   ^definition.class
  case Green
  //   ^definition.class
  case Yellow
  //   ^definition.class

trait Fruit:
//    ^definition.interface
  val color: Color
//    ^definition.variable

object Fruit:
//     ^definition.object
  case object Banana extends Fruit {
  //          ^definition.object
  //                         ^reference.class
    val color = Color.Yellow
    //  ^definition.variable
  }
  case class Apple(c: Color.Red | Color.Green) extends Fruit {
  //         ^definition.class
  //               ^definition.property
  //                                                   ^reference.class
    val color = c
  //    ^definition.variable
  }

  given show: Show[Fruit] = new Show[Fruit] {  }
  //    ^definition.variable
  //                            ^reference.interface

  def color(fruit: Fruit): String = ???
  //  ^definition.function

  var flag: Boolean = true
  //  ^definition.variable


