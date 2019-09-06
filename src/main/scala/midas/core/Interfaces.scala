// See LICENSE for license details.

package midas
package core

import midas.widgets.{EndpointIOAnnotation, HasChannels}

import chisel3._
import chisel3.util.ReadyValidIO

import freechips.rocketchip.util.{DecoupledHelper}

import scala.collection.mutable

// Adapted from DecoupledIO in Chisel3
class HostDecoupledIO[+T <: Data](gen: T) extends Bundle
{
  val hReady = Input(Bool())
  val hValid = Output(Bool())
  val hBits  = gen.cloneType
  def fire(): Bool = hReady && hValid
  override def cloneType: this.type =
    new HostDecoupledIO(gen).asInstanceOf[this.type]
}

/** Adds a ready-valid handshaking protocol to any interface.
  * The standard used is that the consumer uses the flipped interface.
  */
object HostDecoupled {
  def apply[T <: Data](gen: T): HostDecoupledIO[T] = new HostDecoupledIO(gen)
}

class HostReadyValid extends Bundle {
  val hReady = Input(Bool())
  val hValid = Output(Bool())
  def fire(): Bool = hReady && hValid
}

// For use in the interface of EndpointWidgets
// We're using a Record here because reflection in Bundle prematurely initializes our lazy vals
class HostPortIO[+T <: Data](protected val targetPortProto: T) extends Record with HasChannels {
  val fromHost = new HostReadyValid
  val toHost = Flipped(new HostReadyValid)
  val hBits  = targetPortProto

  val elements = collection.immutable.ListMap(Seq("fromHost" -> fromHost, "toHost" -> toHost, "hBits" -> hBits):_*)

  override def cloneType: this.type = new HostPortIO(targetPortProto).asInstanceOf[this.type]

  // These are lazy because parsePorts needs a directioned gen; these can be called once 
  // this Record has been bound to Hardware
  private lazy val (ins, outs, rvIns, rvOuts) = SimUtils.parsePorts(targetPortProto, alsoFlattenRVPorts = false)
  def inputWireChannels(): Seq[(Data, String)] = ins
  def outputWireChannels(): Seq[(Data, String)] = outs
  def inputRVChannels(): Seq[(ReadyValidIO[Data], String)] = rvIns
  def outputRVChannels(): Seq[(ReadyValidIO[Data], String)] = rvOuts
  lazy val name2Wire = Map((ins ++ outs).map({ case (wire, name) => name -> wire }):_*)
  lazy val name2ReadyValid = Map((rvIns ++ rvOuts).map({ case (wire, name) => name -> wire }):_*)

  def connectChannels2Port(endpointAnno: EndpointIOAnnotation, simIo: SimWrapperChannels): Unit = {
    val local2globalName = endpointAnno.channelMapping.toMap
    val toHostChannels, fromHostChannels = mutable.ArrayBuffer[ReadyValidIO[Data]]()

    // Bind payloads to HostPort, and collect channels
    for ((field, localName) <- inputWireChannels) {
      val tokenChannel = simIo.wireOutputPortMap(local2globalName(localName))
      field := tokenChannel.bits
      toHostChannels += tokenChannel
    }

    for ((field, localName) <- outputWireChannels) {
      val tokenChannel = simIo.wireInputPortMap(local2globalName(localName))
      tokenChannel.bits := field
      fromHostChannels += tokenChannel
    }

    for ((field, localName) <- inputRVChannels) {
      val (fwdChPort, revChPort) = simIo.rvOutputPortMap(local2globalName(localName + "_fwd"))
      field.valid := fwdChPort.bits.valid
      revChPort.bits := field.ready

      import chisel3.core.ExplicitCompileOptions.NotStrict
      field.bits  := fwdChPort.bits.bits

      fromHostChannels += revChPort
      toHostChannels += fwdChPort
    }

    for ((field, localName) <- outputRVChannels) {
      val (fwdChPort, revChPort) = simIo.rvInputPortMap(local2globalName(localName + "_fwd"))
      fwdChPort.bits.valid := field.valid
      field.ready := revChPort.bits

      import chisel3.core.ExplicitCompileOptions.NotStrict
      fwdChPort.bits.bits := field.bits
      fromHostChannels += fwdChPort
      toHostChannels += revChPort
    }

    toHost.hValid := toHostChannels.foldLeft(true.B)(_ && _.valid)
    fromHost.hReady := fromHostChannels.foldLeft(true.B)(_ && _.ready)

    // Dequeue from toHost channels only if all toHost tokens are available,
    // and the endpoint consumes it
    val toHostHelper   = DecoupledHelper((toHost.hReady +: toHostChannels.map(_.valid)):_*)
    toHostChannels.foreach(ch => ch.ready := toHostHelper.fire(ch.valid))

    // Enqueue into the toHost channels only once all toHost channels can accept the token
    val fromHostHelper = DecoupledHelper((fromHost.hValid +: fromHostChannels.map(_.ready)):_*)
    fromHostChannels.foreach(ch => ch.valid := fromHostHelper.fire(ch.ready))
  }

  def generateAnnotations(): Unit = {
    generateWireChannelFCCAs(inputWireChannels, endpointSunk = true, latency = 1)
    generateWireChannelFCCAs(outputWireChannels, endpointSunk = false, latency = 1)
    generateRVChannelFCCAs(inputRVChannels, endpointSunk = true)
    generateRVChannelFCCAs(outputRVChannels, endpointSunk = false)
  }
}

object HostPort {
  def apply[T <: Data](gen: T): HostPortIO[T] = new HostPortIO(gen)
}
