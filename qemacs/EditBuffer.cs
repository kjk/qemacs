using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Formatters;
using System.Runtime.Serialization.Formatters.Binary;

namespace qemacs
{
    public static class EditBufferDataTypes
    {
        public static List<IEditBufferDataType> DataTypes = new List<IEditBufferDataType>() { new EditBufferDataTypeRaw() };
        public static void RegisterDataType(IEditBufferDataType dt)
        {
            DataTypes.Add(dt);
        }
    }

    public class EditBuffer
    {
        [Flags]
        public enum BufferFlags {
            SaveLog  = 0x0001,  /* activate buffer logging */
            System   = 0x0002,  /* buffer system, cannot be seen by the user */
            ReadOnly = 0x0004,  /* read only buffer */
            Preview  = 0x0008,  /* used in dired mode to mark previewed files */
            Loading  = 0x0010,  /* buffer is being loaded */
            Saving   = 0x0020,  /* buffer is being saved */
            Dired    = 0x0100  /* buffer is interactive dired */
        }
 
        public enum LogOp {
            Free = 0,
            Write,
            Insert,
            Delete
        }

        [Serializable]
        public class LogBuffer
        {
            public LogOp op;
            public bool wasModified;
            public int offset;
            public int size;
        }

        public const int NB_LOGS_MAX = 50;

        Pages pages = new Pages();
        int TotalSize { get { return pages.total_size; } }
        bool modified = false;
        EditBuffer log_buffer;
        int nb_logs = 0;
        BufferFlags flags;
        string name;

        void SetFlag(BufferFlags f)
        {
            flags |= f;
        }

        bool IsFlagSet(BufferFlags f)
        {
            return (flags & f) != 0;
        }

        void ClearFlag(BufferFlags f)
        {
            flags &= ~f;
        }

        void SetOrClearFlag(bool set, BufferFlags f)
        {
            if (set)
                SetFlag(f);
            else
                ClearFlag(f);
        }

        bool save_log 
        {
            get { return IsFlagSet(BufferFlags.SaveLog); }
            set { SetOrClearFlag(value, BufferFlags.SaveLog); }
        }

        public EditBuffer(string name, BufferFlags flags)
        {
            this.name = name;
            this.flags = flags;
        }

        void LimitLogSize()
        {
            if (nb_logs < NB_LOGS_MAX)
                return;
            // TODO: implement me
        }

        void AddLog(LogOp op, int offset, int size)
        {
            // TODO: call callbacks
            bool was_modified = modified;
            modified = true;
            if (!save_log)
                return;

            if (null == log_buffer)
                log_buffer = new EditBuffer(String.Format("*log <{0}>", name), BufferFlags.System);
            LimitLogSize();
            // TODO: finish me
        }

        void VerifyOffset(int offset)
        {
            Debug.Assert(offset >= 0);
            Debug.Assert(offset <= TotalSize);
        }

        /* Insert 'size' bytes from 'buf' into 'b' at offset 'offset'. We must
           have : 0 <= offset <= b->total_size */
        public void Insert(int offset, byte[] buf, int size)
        {
            VerifyOffset(offset);
            AddLog(LogOp.Insert, offset, size);
            pages.InsertLowLevel(offset, buf, size);
        }
    }
}
