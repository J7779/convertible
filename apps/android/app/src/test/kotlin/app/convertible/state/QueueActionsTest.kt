package app.convertible.state

import app.convertible.model.TargetInfo
import org.junit.Assert.assertEquals
import org.junit.Test

class QueueActionsTest {
    private val output = ConversionOutput("/w/out.txt", "out.txt", 3, "txt", "txt", "text/plain", emptyList())
    private val target = TargetInfo(id = "txt", name = "Text", category = "document", extensions = listOf("txt"), mime = "text/plain", lossy = false, compress = false)

    private fun item(id: Long, status: ItemStatus) =
        QueueItem(id = id, name = "f$id", size = 1, inputPath = "/w/f$id", sourceId = "bin", target = target, status = status)

    @Test
    fun `two done and one converting keeps Cancel all in the phone bar`() {
        val state = QueueState(
            items = listOf(
                item(1, ItemStatus.Done(output)),
                item(2, ItemStatus.Done(output)),
                item(3, ItemStatus.Converting(0.4f, "Encoding")),
            ),
        )
        assertEquals(listOf(QueueAction.CANCEL_ALL, QueueAction.SAVE_ALL, QueueAction.SHARE_ALL), state.queueActions())
        assertEquals(listOf(QueueAction.CANCEL_ALL, QueueAction.SAVE_ALL), state.queueActions(limit = 2))
    }

    @Test
    fun `waiting files outrank saving while something runs`() {
        val state = QueueState(
            items = listOf(
                item(1, ItemStatus.Done(output)),
                item(2, ItemStatus.Done(output)),
                item(3, ItemStatus.Queued),
                item(4, ItemStatus.Ready),
            ),
        )
        assertEquals(listOf(QueueAction.CANCEL_ALL, QueueAction.CONVERT_ALL), state.queueActions(limit = 2))
        assertEquals(4, state.queueActions().size)
    }

    @Test
    fun `finished queue offers save and share, idle queue offers convert`() {
        assertEquals(
            listOf(QueueAction.SAVE_ALL, QueueAction.SHARE_ALL),
            QueueState(items = listOf(item(1, ItemStatus.Done(output)))).queueActions(limit = 2),
        )
        assertEquals(listOf(QueueAction.CONVERT_ALL), QueueState(items = listOf(item(1, ItemStatus.Ready))).queueActions(limit = 2))
        assertEquals(emptyList<QueueAction>(), QueueState().queueActions())
    }
}
